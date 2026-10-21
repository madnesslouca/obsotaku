/* Real dock and libobs settings, with isolated app/config adapters. The media
 * input is a settings-only stub: this suite does not decode or certify audio. */
#include <docks/StudioClipsDock.hpp>
#include <widgets/OBSBasic.hpp>
#include <obs.hpp>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QSaveFile>
#include <QShowEvent>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

QString clipLibraryConfigRoot;
static QJsonArray checks;
void OBSDock::closeEvent(QCloseEvent *event)
{
	QDockWidget::closeEvent(event);
}
void OBSDock::showEvent(QShowEvent *event)
{
	QDockWidget::showEvent(event);
}
static void check(bool value, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", value}});
	if (!value) {
		throw std::runtime_error(name);
	}
}
static QString media(const QString &name)
{
	const QString path = clipLibraryConfigRoot + QLatin1Char('/') + name;
	QFile file(path);
	check(file.open(QIODevice::WriteOnly) && file.write("fixture") == 7, "temporary media placeholder created");
	return QFileInfo(path).canonicalFilePath();
}
static QString libraryPath()
{
	return clipLibraryConfigRoot + QStringLiteral("/obs-studio/plugin_config/studio-clips.json");
}
static QJsonObject readLibrary()
{
	QFile file(libraryPath());
	check(file.open(QIODevice::ReadOnly), "library can be read");
	return QJsonDocument::fromJson(file.readAll()).object();
}
static void writeLibrary(const QJsonObject &object)
{
	QDir().mkpath(QFileInfo(libraryPath()).absolutePath());
	QSaveFile file(libraryPath());
	const auto bytes = QJsonDocument(object).toJson();
	check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
	      "isolated library fixture committed");
}
static QListWidget *list(StudioClipsDock &dock)
{
	return dock.findChild<QListWidget *>(QStringLiteral("studioClipLibrary"));
}
static QJsonObject entry(const QJsonObject &root, const QString &path)
{
	for (const auto value : root.value("clips").toArray()) {
		const auto object = value.toObject();
		if (object.value("path").toString() == path) {
			return object;
		}
	}
	return {};
}
static OBSSourceAutoRelease source(const char *name, const char *canvas, bool managed, bool follow)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "local_file", "original-file");
	OBSSourceAutoRelease result = obs_source_create("ffmpeg_source", name, settings, nullptr);
	check(bool(result), "settings-only libobs media source created");
	OBSDataAutoRelease metadata = obs_source_get_private_settings(result);
	obs_data_set_bool(metadata, "studio_replay_managed", managed);
	obs_data_set_bool(metadata, "studio_replay_follow", follow);
	obs_data_set_int(metadata, "studio_replay_a", 100);
	obs_data_set_int(metadata, "studio_replay_b", 900);
	if (canvas) {
		obs_data_set_string(metadata, "studio_replay_canvas", canvas);
	}
	return result;
}
static QString path(obs_source_t *source)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	return QString::fromUtf8(obs_data_get_string(settings, "local_file"));
}
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QTemporaryDir temporary;
	clipLibraryConfigRoot = temporary.path() + QStringLiteral("/Clipes ação");
	QDir().mkpath(clipLibraryConfigRoot);
	bool started = false;
	QString error;
	try {
		check(temporary.isValid(), "private temporary directory valid");
		check(obs_startup("en-US", nullptr, nullptr), "isolated libobs startup");
		started = true;
		obs_source_info info{};
		info.id = "ffmpeg_source";
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.get_name = [](void *) {
			return "Settings-only media fixture";
		};
		info.create = [](obs_data_t *, obs_source_t *) -> void * {
			return new int(0);
		};
		info.destroy = [](void *data) {
			delete static_cast<int *>(data);
		};
		obs_register_source(&info);
		const QString initial = media(QStringLiteral("legado.mkv"));
		const QString missing = clipLibraryConfigRoot + QStringLiteral("/ausente.mkv");
		const QString horizontal = media(QStringLiteral("main.mkv"));
		const QString vertical = media(QStringLiteral("portrait.mkv"));
		writeLibrary({{"version", 1}, {"clips", QJsonArray{initial, missing}}});
		auto legacy = source("legacy-follow", nullptr, true, true);
		auto main = source("main-follow", "main", true, true);
		auto portrait = source("portrait-follow", "portrait", true, true);
		auto fixed = source("fixed-replay", "portrait", true, false);
		auto unmanaged = source("ordinary-media", "portrait", false, true);
		{
			StudioClipsDock dock;
			check(list(dock)->count() == 2, "v1 present and missing entries both load");
			check(dock.CanvasForClip(initial) == "main" && dock.CanvasForClip(missing) == "main",
			      "v1 entries acquire legacy Main origin");
			dock.RegisterCanvasClip(vertical, "portrait", "pair-a");
			check(path(portrait) == vertical, "Portrait save retargets Portrait follower");
			check(path(main) == "original-file" && path(legacy) == "original-file",
			      "Portrait save preserves explicit and legacy Main followers");
			check(path(fixed) == "original-file" && path(unmanaged) == "original-file",
			      "fixed and unmanaged sources are never retargeted");
			OBSDataAutoRelease portraitMeta = obs_source_get_private_settings(portrait);
			check(obs_data_get_int(portraitMeta, "studio_replay_a") == 0 &&
				      obs_data_get_int(portraitMeta, "studio_replay_b") == 0,
			      "changed follower timeline clears old A/B markers");
			check(list(dock)->item(0)->data(Qt::UserRole + 1).toString() == "portrait" &&
				      list(dock)->item(0)->data(Qt::UserRole + 2).toString() == "pair-a" &&
				      list(dock)->item(0)->toolTip().contains("pair-a") &&
				      list(dock)->item(0)->text().contains("Studio.Canvas.PortraitChoice"),
			      "list displays and retains canvas and pair metadata");
			dock.RegisterCanvasClip(horizontal, "main", "pair-a");
			check(path(main) == horizontal && path(legacy) == horizontal,
			      "Main save retargets explicit and legacy Main followers");
			check(path(portrait) == vertical, "later Main completion cannot flip Portrait follower");
			check(dock.CanvasForClip(horizontal) == "main" && dock.CanvasForClip(vertical) == "portrait",
			      "canvas lookup follows file origin independently of list order");
			auto saved = readLibrary();
			check(saved.value("version").toInt() == 2 && saved.value("clips").toArray().size() == 4,
			      "first save migrates v1 to v2 without dropping entries");
			check(entry(saved, missing).value("canvas").toString() == "main",
			      "missing v1 path survives migration");
			check(entry(saved, vertical).value("pairId").toString() == "pair-a" &&
				      entry(saved, horizontal).value("pairId").toString() == "pair-a",
			      "both files preserve the same save-pair identity");
			dock.Shutdown();
		}
		{
			StudioClipsDock dock;
			check(list(dock)->count() == 4 && dock.CanvasForClip(vertical) == "portrait",
			      "v2 reload preserves missing paths and Portrait origin");
			check(list(dock)->item(1)->data(Qt::UserRole + 2).toString() == "pair-a",
			      "v2 reload preserves pair metadata and order");
			const QString verticalLater = media(QStringLiteral("portrait-later.mkv"));
			dock.RegisterCanvasClip(verticalLater, "portrait", "pair-b");
			check(path(portrait) == verticalLater && path(main) == horizontal,
			      "next Portrait-only save leaves Main running on its own latest file");
			dock.RegisterClip(initial);
			check(path(main) == initial && path(legacy) == initial && path(portrait) == verticalLater,
			      "legacy RegisterClip remains Main-only");
			check(dock.CanvasForClip(clipLibraryConfigRoot + "/unknown.mkv") == "main",
			      "unknown import defaults to Main");
			dock.RegisterCanvasClip(missing, "portrait", "invalid");
			check(dock.CanvasForClip(missing) == "main",
			      "missing new saved clip cannot overwrite library metadata");
			for (int i = 0; i < 205; ++i) {
				const QString file = media(QStringLiteral("limit-%1.mkv").arg(i));
				dock.RegisterCanvasClip(file, i % 2 ? "portrait" : "main", "limit-pair");
			}
			check(list(dock)->count() == 200 && list(dock)->item(0)->text().startsWith("limit-204.mkv"),
			      "library retains only the newest 200 entries");
			check(readLibrary().value("clips").toArray().size() == 200,
			      "v2 persisted library respects the same 200-entry cap");
			const int saves = OBSBasic::Get()->saves;
			dock.Shutdown();
			dock.RegisterCanvasClip(vertical, "portrait", "after-close");
			check(OBSBasic::Get()->saves == saves && list(dock)->count() == 200,
			      "shutdown rejects late saved callbacks");
		}
		portrait = nullptr;
		main = nullptr;
		legacy = nullptr;
		fixed = nullptr;
		unmanaged = nullptr;
	} catch (const std::exception &exception) {
		error = QString::fromUtf8(exception.what());
	}
	if (started) {
		obs_shutdown();
	}
	const QJsonObject result{
		{"passed", error.isEmpty()},
		{"error", error},
		{"checks", checks},
		{"scope",
		 "Real Qt dock and libobs source settings with isolated app/config adapters; fake media input, no decoding/GPU/Windows/insertion UI test"}};
	if (argc > 1) {
		QFile report(QString::fromLocal8Bit(argv[1]));
		if (report.open(QIODevice::WriteOnly)) {
			report.write(QJsonDocument(result).toJson());
		}
	}
	std::printf("%s (%lld checks)%s\n", error.isEmpty() ? "PASS" : "FAIL", (long long)checks.size(),
		    qPrintable(error.isEmpty() ? QString() : ": " + error));
	return error.isEmpty() ? 0 : 1;
}
