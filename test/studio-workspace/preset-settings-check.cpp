/* Opt-in native UI regression. Gesture: edit an unrelated bitrate and Apply,
 * then edit resolution and Cancel. Valid externally configured resolutions
 * must remain visible after the periodic UI refresh, persist unchanged in the
 * profile on Apply, and survive reopening. No media output is started. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/bmem.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
OBS_DECLARE_MODULE()
namespace {
QString root, originalProfile, qaProfile, profilePath;
QMainWindow *window = nullptr;
QJsonArray checks, observations;
bool done = false, duplicated = false, guarded = false;
int phase = 0;
struct Resolution {
	const char *key;
	const char *control;
	const char *value;
	const char *stock;
	bool data;
};
const Resolution resolutions[] = {
	{"MainStreamResolution", "studioCanvasOutputResolution", "1600x900", "1280x720", false},
	{"MainReplayResolution", "studioCanvasMainReplayResolution", "1536x864", "1280x720", true},
	{"PortraitReplayResolution", "studioCanvasPortraitReplayResolution", "864x1536", "720x1280", true}};
void finish(QString error = {});
void check(bool passed, const char *name)
{
	checks.append(QJsonObject{{"check", name},
				  {"passed", passed},
				  {"phase", phase},
				  {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
	QSaveFile f(root + "/progress.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"checks", checks}, {"last_check", name}, {"phase", phase}}).toJson());
		f.commit();
	}
	if (!passed) {
		throw std::runtime_error(name);
	}
}
void later(int ms, std::function<void()> fn)
{
	QTimer::singleShot(ms, qApp, [fn] {
		if (done) {
			return;
		}
		try {
			fn();
		} catch (const std::exception &e) {
			finish(QString::fromUtf8(e.what()));
		}
	});
}
template<class T> T *child(QObject *parent, const char *name)
{
	auto *c = parent->findChild<T *>(name);
	check(c != nullptr, name);
	return c;
}
QString currentProfile()
{
	char *p = obs_frontend_get_current_profile();
	QString value = QString::fromUtf8(p);
	bfree(p);
	return value;
}
void report(const QString &error)
{
	QSaveFile f(root + "/runtime.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"observations", observations},
					{"original_profile", originalProfile},
					{"temporary_profile", qaProfile},
					{"private_config_guard", guarded},
					{"scope",
					 "Native Settings UI with periodic refresh, profile-memory and disk persistence, reopen and Cancel; Simple and Advanced. No encoder/media output started; Linux execution does not certify Windows."}})
				.toJson());
		f.commit();
	}
}
void restoreProfile(QString error, std::shared_ptr<QElapsedTimer> clock)
{
	if (currentProfile() != originalProfile) {
		if (clock->elapsed() >= 10000) {
			report(error + "; original private profile failed to restore, QA profile kept");
			return;
		}
		QTimer::singleShot(100, qApp, [error, clock] { restoreProfile(error, clock); });
		return;
	}
	if (duplicated) {
		obs_frontend_delete_profile(qaProfile.toUtf8().constData());
		duplicated = false;
	}
	obs_frontend_save();
	try {
		check(currentProfile() == originalProfile, "original private profile is restored after regression");
		bool remains = false;
		char **profiles = obs_frontend_get_profiles();
		for (size_t i = 0; profiles && profiles[i]; ++i) {
			remains |= QString::fromUtf8(profiles[i]) == qaProfile;
		}
		bfree(profiles);
		check(!remains, "temporary regression profile is removed");
		check(!obs_video_active() && !obs_frontend_streaming_active() && !obs_frontend_recording_active() &&
			      !obs_frontend_replay_buffer_active(),
		      "no media output was started by the settings regression");
	} catch (const std::exception &e) {
		if (error.isEmpty()) {
			error = QString::fromUtf8(e.what());
		}
	}
	report(error);
}
void finish(QString error)
{
	if (done) {
		return;
	}
	done = true;
	if (auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
		if (d->objectName() == "OBSBasicSettings") {
			if (auto *box = d->findChild<QDialogButtonBox *>("buttonBox")) {
				box->button(QDialogButtonBox::Cancel)->click();
			}
		}
	}
	if (!duplicated) {
		report(error);
		return;
	}
	obs_frontend_set_current_profile(originalProfile.toUtf8().constData());
	auto clock = std::make_shared<QElapsedTimer>();
	clock->start();
	QTimer::singleShot(200, qApp, [error, clock] { restoreProfile(error, clock); });
}
void isolation()
{
	const auto requested = qEnvironmentVariable("OBS_STUDIO_PRESET_SETTINGS_CONFIG_ROOT");
	const auto expected = QFileInfo(requested).canonicalFilePath();
	check(!requested.isEmpty() && !expected.isEmpty() && QFileInfo(expected).isDir(),
	      "explicit private config root exists before any mutation");
	char *path = obs_frontend_get_current_profile_path();
	const auto actual = QFileInfo(QString::fromUtf8(path)).canonicalFilePath();
	bfree(path);
	check(!actual.isEmpty() && actual.startsWith(expected + "/"),
	      "effective profile is inside private root before any mutation");
	auto *app = obs_frontend_get_app_config();
	QJsonObject locations;
	for (const char *key : {"Configuration", "SceneCollections", "Profiles", "PluginManagerSettings"}) {
		const auto raw = QString::fromUtf8(config_get_string(app, "Locations", key));
		locations[key] = raw;
		check(!raw.isEmpty() && QFileInfo(raw).canonicalFilePath() == expected,
		      "all four absolute Locations target the explicitly private root");
	}
	observations.append(
		QJsonObject{{"private_root", expected}, {"initial_profile_path", actual}, {"locations", locations}});
	guarded = true;
}
void verifyConfig(const char *when)
{
	auto *c = obs_frontend_get_profile_config();
	QJsonObject values{{"when", when}, {"phase", phase}};
	for (const auto &r : resolutions) {
		const auto value = QString::fromUtf8(config_get_string(c, "StudioCanvas", r.key));
		values[r.key] = value;
		check(value == r.value, "each nonstandard resolution survives in the effective profile configuration");
	}
	check(config_get_int(c, "StudioCanvas", "MainReplayBitrate") == 4501 + phase,
	      "only the deliberately edited replay bitrate changes");
	for (const char *key : {"MainReplayPresetSource", "PortraitReplayPresetSource"}) {
		check(QString::fromUtf8(config_get_string(c, "StudioCanvas", key)) == "custom",
		      "custom replay preset origins remain preserved");
	}
	config_t *disk = nullptr;
	const int result = config_open(&disk, (profilePath + "/basic.ini").toUtf8().constData(), CONFIG_OPEN_EXISTING);
	check(result == CONFIG_SUCCESS && disk, "native profile file can be reopened from disk after Apply");
	bool valid = true;
	for (const auto &r : resolutions) {
		valid &= QString::fromUtf8(config_get_string(disk, "StudioCanvas", r.key)) == r.value;
	}
	valid &= config_get_int(disk, "StudioCanvas", "MainReplayBitrate") == 4501 + phase;
	config_close(disk);
	check(valid, "all three resolution values and edited bitrate persist in the actual profile file");
	observations.append(values);
}
void verifyControls(QDialog *d)
{
	QJsonObject shown{{"phase", phase}, {"after_periodic_refresh_ms", 900}};
	for (const auto &r : resolutions) {
		auto *combo = child<QComboBox>(d, r.control);
		const auto value = r.data ? combo->currentData().toString() : combo->currentText();
		shown[r.key] = value;
		check(value == r.value, "native resolution selector shows the externally configured nonstandard value");
		if (r.data) {
			int count = 0;
			for (int i = 0; i < combo->count(); ++i) {
				count += combo->itemData(i).toString() == r.value;
			}
			check(count == 1,
			      "external replay resolution is represented once without duplicate menu entries");
		}
	}
	observations.append(shown);
}
void edit(std::function<void(QDialog *)> fn)
{
	auto finished = std::make_shared<bool>(false);
	// Studio refreshes its native proxy controls periodically. Waiting more than
	// one refresh prevents a transient value before the timer from passing.
	later(900, [fn, finished] {
		auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
		check(d && d->objectName() == "OBSBasicSettings", "native output Settings is open");
		child<QListWidget>(d, "listWidget")->setCurrentRow(0);
		fn(d);
		child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
		*finished = true;
	});
	child<QAction>(window, "action_Settings")->trigger();
	check(*finished || done, "native Settings completes without a save-confirmation modal");
}
void apply(QDialog *d)
{
	auto *button = child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply);
	check(button->isEnabled(), "a deliberate unrelated edit enables native Apply");
	button->click();
}
void exercise()
{
	auto *c = obs_frontend_get_profile_config();
	config_set_string(c, "Output", "Mode", phase ? "Advanced" : "Simple");
	for (const auto &r : resolutions) {
		config_set_string(c, "StudioCanvas", r.key, r.value);
	}
	config_set_string(c, "StudioCanvas", "MainReplayPresetSource", "custom");
	config_set_string(c, "StudioCanvas", "PortraitReplayPresetSource", "custom");
	config_set_int(c, "StudioCanvas", "MainReplayBitrate", 4499);
	check(config_save_safe(c, "tmp", nullptr) == CONFIG_SUCCESS,
	      "temporary profile stores the externally provided resolution values");
	edit([](QDialog *d) {
		verifyControls(d);
		check(child<QComboBox>(d, "outputMode")->currentIndex() == phase,
		      "native Simple or Advanced output mode loads");
		child<QSpinBox>(d, "studioCanvasMainReplayBitrate")->setValue(4501 + phase);
		apply(d);
		verifyConfig("unrelated-bitrate-apply");
	});
	if (done) {
		return;
	}
	edit([](QDialog *d) {
		verifyControls(d);
		check(child<QSpinBox>(d, "studioCanvasMainReplayBitrate")->value() == 4501 + phase,
		      "edited bitrate survives reopening Settings");
		for (const auto &r : resolutions) {
			if (r.data) {
				auto *combo = child<QComboBox>(d, r.control);
				combo->setCurrentIndex(combo->findData(""));
				combo->setCurrentIndex(combo->findData(r.value));
			}
		}
		apply(d);
		verifyConfig("resolution-roundtrip-apply");
	});
	if (done) {
		return;
	}
	edit([](QDialog *d) {
		verifyControls(d);
		for (const auto &r : resolutions) {
			auto *combo = child<QComboBox>(d, r.control);
			const int index = r.data ? combo->findData(r.stock) : combo->findText(r.stock);
			check(index >= 0, "stock resolution for the Cancel test exists");
			combo->setCurrentIndex(index);
			if (!r.data) {
				check(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection,
								Q_ARG(int, index)),
				      "Main stream resolution receives the native activation gesture");
			}
		}
		// The native Cancel button clears dirty flags and closes without saving.
	});
	if (done) {
		return;
	}
	verifyConfig("cancelled-resolution-edits");
	edit([](QDialog *d) { verifyControls(d); });
	if (done) {
		return;
	}
	if (++phase < 2) {
		later(200, exercise);
	} else {
		finish();
	}
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active() && !obs_frontend_streaming_active() && !obs_frontend_recording_active() &&
		      !obs_frontend_replay_buffer_active(),
	      "OBS is idle before isolated settings-only regression");
	isolation();
	originalProfile = currentProfile();
	qaProfile = "Preset Settings QA " + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");
	obs_frontend_duplicate_profile(qaProfile.toUtf8().constData());
	duplicated = true;
	later(600, [] {
		check(currentProfile() == qaProfile, "regression uses only its own duplicated profile");
		char *p = obs_frontend_get_current_profile_path();
		profilePath = QString::fromUtf8(p);
		bfree(p);
		exercise();
	});
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1500, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_PRESET_SETTINGS_ROOT");
	if (root.isEmpty() || QFileInfo::exists(root + "/runtime.json") || !QDir().mkpath(root)) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	QTimer::singleShot(30000, qApp, [] {
		if (!done) {
			finish("settings-only regression exceeded 30-second deadline");
		}
	});
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
