#include "StudioClipsDock.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <obs.hpp>

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

namespace {
constexpr int CanvasRole = Qt::UserRole + 1;
constexpr int PairIdRole = Qt::UserRole + 2;

QString CanvasId(const QString &canvas)
{
	return canvas == QStringLiteral("portrait") ? canvas : QStringLiteral("main");
}

QString ClipPath(const QString &path)
{
	const QFileInfo file(path);
	const QString canonical = file.canonicalFilePath();
	return canonical.isEmpty() ? QDir::cleanPath(file.absoluteFilePath()) : canonical;
}

QString CanvasLabel(const QString &canvas)
{
	return QTStr(canvas == QStringLiteral("portrait") ? "Studio.Canvas.PortraitChoice"
							  : "Studio.Canvas.MainChoice");
}
} // namespace

static bool Managed(obs_source_t *source)
{
	if (!source || obs_source_removed(source) || strcmp(obs_source_get_unversioned_id(source), "ffmpeg_source")) {
		return false;
	}
	OBSDataAutoRelease data = obs_source_get_private_settings(source);
	return obs_data_get_bool(data, "studio_replay_managed");
}

static QString Time(int64_t ms)
{
	ms = std::max<int64_t>(0, ms);
	return QStringLiteral("%1:%2.%3")
		.arg(ms / 60000, 2, 10, QLatin1Char('0'))
		.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
		.arg((ms % 1000) / 100, 1, 10, QLatin1Char('0'));
}

StudioClipsDock::StudioClipsDock(QWidget *parent) : OBSDock(parent)
{
	setObjectName(QStringLiteral("studioClipsDock"));
	setWindowTitle(QTStr("Studio.Clips"));
	setMinimumWidth(280);
	auto *body = new QWidget(this);
	auto *layout = new QVBoxLayout(body);
	clips = new QListWidget(body);
	clips->setObjectName(QStringLiteral("studioClipLibrary"));
	clips->setMaximumHeight(180);
	layout->addWidget(clips, 1);
	auto *files = new QHBoxLayout;
	auto *import = new QPushButton(QTStr("Studio.ClipImport"), body);
	import->setObjectName(QStringLiteral("studioClipImport"));
	auto *open = new QPushButton(QTStr("Studio.ClipFolder"), body);
	auto *forget = new QPushButton(QTStr("Studio.ClipForget"), body);
	forget->setToolTip(QTStr("Studio.ClipForgetHint"));
	files->addWidget(import);
	files->addWidget(open);
	files->addWidget(forget);
	layout->addLayout(files);
	auto *insertFollow = new QCheckBox(QTStr("Studio.ClipFollow"), body);
	insertFollow->setObjectName(QStringLiteral("studioClipInsertFollow"));
	insertFollow->setChecked(true);
	layout->addWidget(insertFollow);
	insert = new QPushButton(QTStr("Studio.ClipInsert"), body);
	insert->setObjectName(QStringLiteral("studioClipInsert"));
	layout->addWidget(insert);
	layout->addWidget(new QLabel(QTStr("Studio.ClipLayers"), body));
	layers = new QListWidget(body);
	layers->setObjectName(QStringLiteral("studioReplayLayers"));
	layers->setMaximumHeight(90);
	layout->addWidget(layers);
	playback = new QWidget(body);
	auto *controls = new QVBoxLayout(playback);
	controls->setContentsMargins(0, 0, 0, 0);
	play = new QPushButton(QTStr("Studio.ClipPlay"), playback);
	play->setObjectName(QStringLiteral("studioReplayPlay"));
	controls->addWidget(play);
	seek = new QSlider(Qt::Horizontal, playback);
	seek->setObjectName(QStringLiteral("studioReplaySeek"));
	seek->setRange(0, 1000);
	seek->setToolTip(QTStr("Studio.ClipSeekHint"));
	controls->addWidget(seek);
	auto *speeds = new QHBoxLayout;
	speeds->addWidget(new QLabel(QTStr("Studio.ClipSpeed"), playback));
	speed = new QDoubleSpinBox(playback);
	speed->setObjectName(QStringLiteral("studioReplaySpeed"));
	speed->setRange(0.3, 2.0);
	speed->setSingleStep(0.1);
	speed->setDecimals(2);
	speed->setSuffix(QStringLiteral(" ×"));
	speeds->addWidget(speed);
	controls->addLayout(speeds);
	follow = new QCheckBox(QTStr("Studio.ClipFollow"), playback);
	follow->setObjectName(QStringLiteral("studioReplayFollow"));
	loop = new QCheckBox(QTStr("Studio.ClipLoop"), playback);
	loop->setObjectName(QStringLiteral("studioReplayLoop"));
	controls->addWidget(follow);
	controls->addWidget(loop);
	auto *marks = new QHBoxLayout;
	auto *a = new QPushButton(QTStr("Studio.ClipMarkA"), playback);
	auto *b = new QPushButton(QTStr("Studio.ClipMarkB"), playback);
	auto *reset = new QPushButton(QTStr("Studio.ClipReset"), playback);
	a->setObjectName(QStringLiteral("studioReplayMarkA"));
	b->setObjectName(QStringLiteral("studioReplayMarkB"));
	reset->setObjectName(QStringLiteral("studioReplayReset"));
	marks->addWidget(a);
	marks->addWidget(b);
	marks->addWidget(reset);
	controls->addLayout(marks);
	range = new QLabel(playback);
	range->setWordWrap(true);
	range->setTextFormat(Qt::PlainText);
	controls->addWidget(range);
	layout->addWidget(playback);
	status = new QLabel(QTStr("Studio.ClipLibraryHint"), body);
	status->setTextFormat(Qt::PlainText);
	status->setWordWrap(true);
	layout->addWidget(status);
	// Keep all playback controls reachable without increasing the main window's
	// minimum height when the library is stacked under another dock.
	auto *scroll = new QScrollArea(this);
	scroll->setObjectName(QStringLiteral("studioClipsScroll"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(body);
	setWidget(scroll);
	char *config = GetAppConfigPathPtr("obs-studio/plugin_config/studio-clips.json");
	storagePath = QString::fromUtf8(config ? config : "");
	bfree(config);
	Load();
	connect(import, &QPushButton::clicked, this, [this]() {
		const auto paths = QFileDialog::getOpenFileNames(
			this, QTStr("Studio.ClipImport"), {},
			QStringLiteral("Video (*.mkv *.mp4 *.mov *.webm *.flv *.ts *.avi)"));
		for (const auto &path : paths) {
			AddFile(path);
		}
		if (!paths.isEmpty()) {
			Save();
		}
	});
	connect(open, &QPushButton::clicked, this, [this]() {
		if (auto *item = clips->currentItem()) {
			QDesktopServices::openUrl(
				QUrl::fromLocalFile(QFileInfo(item->data(Qt::UserRole).toString()).absolutePath()));
		}
	});
	connect(forget, &QPushButton::clicked, this, [this]() {
		delete clips->takeItem(clips->currentRow());
		Save(); // Metadata only. Never delete the recording or alter an existing layer.
	});
	connect(insert, &QPushButton::clicked, this, [this, insertFollow]() {
		if (auto *item = clips->currentItem(); item && QFileInfo::exists(item->data(Qt::UserRole).toString())) {
			emit insertRequested(item->data(Qt::UserRole).toString(), insertFollow->isChecked());
		}
	});
	connect(layers, &QListWidget::currentRowChanged, this, [this]() {
		selectedUUID = layers->currentItem() ? layers->currentItem()->data(Qt::UserRole).toString() : QString();
	});
	connect(play, &QPushButton::clicked, this, [this]() {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (!Managed(source)) {
			return;
		}
		if (obs_source_media_get_state(source) == OBS_MEDIA_STATE_ENDED ||
		    obs_source_media_get_state(source) == OBS_MEDIA_STATE_STOPPED) {
			obs_source_media_restart(source);
			OBSDataAutoRelease data = obs_source_get_private_settings(source);
			obs_source_media_set_time(source, obs_data_get_int(data, "studio_replay_a"));
		} else {
			obs_source_media_play_pause(source,
						    obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING);
		}
	});
	connect(seek, &QSlider::sliderReleased, this, [this]() {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (Managed(source)) {
			obs_source_media_set_time(source, obs_source_media_get_duration(source) * seek->value() / 1000);
		}
	});
	connect(speed, &QDoubleSpinBox::editingFinished, this, [this]() {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (!Managed(source)) {
			return;
		}
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		int percent = qRound(speed->value() * 100);
		if (percent == obs_data_get_int(settings, "speed_percent")) {
			return;
		}
		obs_data_set_int(settings, "speed_percent", percent);
		obs_source_update(source, settings);
		// FFmpeg reopens on a speed change. Reset the range to the new timeline.
		ResetRange();
	});
	connect(follow, &QCheckBox::clicked, this, [this](bool enabled) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (!Managed(source)) {
			return;
		}
		OBSDataAutoRelease data = obs_source_get_private_settings(source);
		obs_data_set_bool(data, "studio_replay_follow", enabled);
		OBSBasic::Get()->SaveProject();
	});
	connect(loop, &QCheckBox::clicked, this, [this](bool enabled) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
		if (!Managed(source)) {
			return;
		}
		OBSDataAutoRelease data = obs_source_get_private_settings(source);
		obs_data_set_bool(data, "studio_replay_loop", enabled);
		OBSBasic::Get()->SaveProject();
	});
	connect(a, &QPushButton::clicked, this, [this]() { Mark(true); });
	connect(b, &QPushButton::clicked, this, [this]() { Mark(false); });
	connect(reset, &QPushButton::clicked, this, &StudioClipsDock::ResetRange);
	timer = new QTimer(this);
	timer->setInterval(100);
	connect(timer, &QTimer::timeout, this, &StudioClipsDock::Tick);
	timer->start();
	playback->setEnabled(false);
}

bool StudioClipsDock::AddFile(const QString &path, const QString &canvas, const QString &pairId, bool allowMissing)
{
	const QFileInfo file(path);
	if (path.isEmpty() || ((!file.isFile() || !file.isReadable()) && (!allowMissing || !file.isAbsolute()))) {
		return false;
	}
	const QString canonical = ClipPath(path);
	QString origin = CanvasId(canvas);
	QString pair = pairId;
	for (int i = clips->count() - 1; i >= 0; --i) {
		if (clips->item(i)->data(Qt::UserRole).toString() == canonical) {
			// Manual re-import preserves any origin already known to the library.
			if (canvas.isEmpty()) {
				origin = CanvasId(clips->item(i)->data(CanvasRole).toString());
				pair = clips->item(i)->data(PairIdRole).toString();
			}
			delete clips->takeItem(i);
		}
	}
	auto *item = new QListWidgetItem(QStringLiteral("%1 · %2").arg(file.fileName(), CanvasLabel(origin)));
	item->setData(Qt::UserRole, canonical);
	item->setData(CanvasRole, origin);
	item->setData(PairIdRole, pair);
	item->setToolTip(
		QStringLiteral("%1\n%2%3")
			.arg(canonical, CanvasLabel(origin),
			     pair.isEmpty() ? QString() : QLatin1Char('\n') + QTStr("Studio.ClipPair").arg(pair)));
	clips->insertItem(0, item);
	clips->setCurrentItem(item);
	while (clips->count() > 200) {
		delete clips->takeItem(clips->count() - 1);
	}
	return true;
}

void StudioClipsDock::Load()
{
	QFile file(storagePath);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
		return;
	}
	const auto root = QJsonDocument::fromJson(file.readAll()).object();
	const int version = root.value("version").toInt();
	if (version != 1 && version != 2) {
		return;
	}
	const auto entries = root.value("clips").toArray();
	for (int i = std::min(entries.size(), qsizetype(200)) - 1; i >= 0; --i) {
		const auto entry = entries.at(i);
		if (version == 1) {
			AddFile(entry.toString(), QStringLiteral("main"), {}, true);
		} else {
			const auto object = entry.toObject();
			AddFile(object.value("path").toString(), CanvasId(object.value("canvas").toString()),
				object.value("pairId").toString(), true);
		}
	}
}

void StudioClipsDock::Save()
{
	if (storagePath.isEmpty()) {
		return;
	}
	QJsonArray entries;
	for (int i = 0; i < clips->count(); ++i) {
		const auto *item = clips->item(i);
		entries.append(QJsonObject{{"path", item->data(Qt::UserRole).toString()},
					   {"canvas", CanvasId(item->data(CanvasRole).toString())},
					   {"pairId", item->data(PairIdRole).toString()}});
	}
	QDir().mkpath(QFileInfo(storagePath).absolutePath());
	QSaveFile file(storagePath);
	const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 2}, {"clips", entries}}).toJson();
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
		status->setText(QTStr("Studio.ClipSaveError"));
	}
}

void StudioClipsDock::RegisterClip(const QString &path)
{
	RegisterCanvasClip(path, QStringLiteral("main"), {});
}

QString StudioClipsDock::CanvasForClip(const QString &path) const
{
	const auto canonical = ClipPath(path);
	for (int i = 0; i < clips->count(); ++i) {
		if (clips->item(i)->data(Qt::UserRole).toString() == canonical) {
			return CanvasId(clips->item(i)->data(CanvasRole).toString());
		}
	}
	return QStringLiteral("main");
}

void StudioClipsDock::RegisterCanvasClip(const QString &path, const QString &canvas, const QString &pairId)
{
	const QString origin = CanvasId(canvas);
	if (closing || !AddFile(path, origin, pairId)) {
		return;
	}
	Save();
	struct FollowClip {
		QByteArray path;
		QString canvas;
	} clip{ClipPath(path).toUtf8(), origin};
	obs_enum_sources(
		[](void *opaque, obs_source_t *source) {
			if (!Managed(source)) {
				return true;
			}
			OBSDataAutoRelease data = obs_source_get_private_settings(source);
			if (!obs_data_get_bool(data, "studio_replay_follow")) {
				return true;
			}
			const auto &clip = *static_cast<const FollowClip *>(opaque);
			if (CanvasId(QString::fromUtf8(obs_data_get_string(data, "studio_replay_canvas"))) !=
			    clip.canvas) {
				return true;
			}
			OBSDataAutoRelease settings = obs_source_get_settings(source);
			const char *path = clip.path.constData();
			if (!strcmp(obs_data_get_string(settings, "local_file"), path)) {
				return true;
			}
			obs_data_set_int(data, "studio_replay_a", 0);
			obs_data_set_int(data, "studio_replay_b", 0);
			obs_data_set_string(settings, "local_file", path);
			obs_source_update(source, settings);
			return true;
		},
		&clip);
	OBSBasic::Get()->SaveProject();
}

void StudioClipsDock::Mark(bool first)
{
	OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
	if (!Managed(source)) {
		return;
	}
	const int64_t duration = obs_source_media_get_duration(source);
	const int64_t time = obs_source_media_get_time(source);
	OBSDataAutoRelease data = obs_source_get_private_settings(source);
	const int64_t a = obs_data_get_int(data, "studio_replay_a");
	const int64_t b = obs_data_get_int(data, "studio_replay_b");
	if (duration <= 0 || (first ? time + 100 >= (b > 0 ? b : duration) : time < a + 100)) {
		return;
	}
	obs_data_set_int(data, first ? "studio_replay_a" : "studio_replay_b", std::clamp<int64_t>(time, 0, duration));
	OBSBasic::Get()->SaveProject();
}

void StudioClipsDock::ResetRange()
{
	OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
	if (!Managed(source)) {
		return;
	}
	OBSDataAutoRelease data = obs_source_get_private_settings(source);
	obs_data_set_int(data, "studio_replay_a", 0);
	obs_data_set_int(data, "studio_replay_b", 0);
	OBSBasic::Get()->SaveProject();
}

void StudioClipsDock::UpdateRange(obs_source_t *source)
{
	const int64_t duration = obs_source_media_get_duration(source);
	const int64_t time = obs_source_media_get_time(source);
	OBSDataAutoRelease data = obs_source_get_private_settings(source);
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	const QSignalBlocker speedBlock(speed), loopBlock(loop), followBlock(follow);
	if (!speed->hasFocus()) {
		speed->setValue(obs_data_get_int(settings, "speed_percent") / 100.0);
	}
	loop->setChecked(obs_data_get_bool(data, "studio_replay_loop"));
	follow->setChecked(obs_data_get_bool(data, "studio_replay_follow"));
	play->setText(QTStr(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING ? "Studio.ClipPause"
											  : "Studio.ClipPlay"));
	if (!seek->isSliderDown()) {
		seek->setValue(duration > 0 ? std::clamp<int64_t>(time * 1000 / duration, 0, 1000) : 0);
	}
	seek->setEnabled(duration > 0);
	const int64_t b = obs_data_get_int(data, "studio_replay_b");
	range->setText(QStringLiteral("%1 / %2\nA %3 — B %4")
			       .arg(Time(time), Time(duration), Time(obs_data_get_int(data, "studio_replay_a")),
				    Time(b > 0 ? b : duration)));
	status->setText(QFileInfo::exists(QString::fromUtf8(obs_data_get_string(settings, "local_file")))
				? QTStr(obs_source_active(source) ? "Studio.ClipOnProgram" : "Studio.ClipInactive")
				: QTStr("Studio.ClipMissing"));
}

void StudioClipsDock::Tick()
{
	if (closing) {
		return;
	}
	std::vector<OBSSource> sources;
	obs_enum_sources(
		[](void *opaque, obs_source_t *source) {
			if (Managed(source)) {
				static_cast<std::vector<OBSSource> *>(opaque)->emplace_back(source);
			}
			return true;
		},
		&sources);
	// Range playback continues when the dock is hidden, only for visible media.
	for (auto &source : sources) {
		OBSDataAutoRelease data = obs_source_get_private_settings(source);
		if (!obs_data_get_bool(data, "studio_replay_loop") || !obs_source_active(source)) {
			continue;
		}
		const int64_t duration = obs_source_media_get_duration(source);
		const int64_t a = std::clamp<int64_t>(obs_data_get_int(data, "studio_replay_a"), 0,
						      std::max<int64_t>(0, duration - 100));
		const int64_t requestedB = obs_data_get_int(data, "studio_replay_b");
		const int64_t b = requestedB > a ? std::min(requestedB, duration) : duration;
		const auto state = obs_source_media_get_state(source);
		if (duration > 0 && b >= a + 100 && state == OBS_MEDIA_STATE_PLAYING &&
		    obs_source_media_get_time(source) >= b - 50) {
			obs_source_media_set_time(source, a);
		} else if (duration > 0 && state == OBS_MEDIA_STATE_ENDED) {
			obs_source_media_restart(source);
			obs_source_media_set_time(source, a);
		}
	}
	if (!isVisible()) {
		return;
	}
	bool changed = layers->count() != int(sources.size());
	for (int i = 0; !changed && i < layers->count(); ++i) {
		changed = layers->item(i)->data(Qt::UserRole).toString() !=
				  QString::fromUtf8(obs_source_get_uuid(sources[i])) ||
			  layers->item(i)->text() != QString::fromUtf8(obs_source_get_name(sources[i]));
	}
	if (changed) {
		const QString previous = selectedUUID;
		layers->clear();
		for (auto &source : sources) {
			auto *item = new QListWidgetItem(QString::fromUtf8(obs_source_get_name(source)), layers);
			item->setData(Qt::UserRole, QString::fromUtf8(obs_source_get_uuid(source)));
			if (item->data(Qt::UserRole).toString() == previous) {
				layers->setCurrentItem(item);
			}
		}
		if (!layers->currentItem() && layers->count()) {
			layers->setCurrentRow(0);
		}
	}
	insert->setEnabled(clips->currentItem() &&
			   QFileInfo::exists(clips->currentItem()->data(Qt::UserRole).toString()));
	OBSSourceAutoRelease source = obs_get_source_by_uuid(selectedUUID.toUtf8().constData());
	playback->setEnabled(Managed(source));
	if (Managed(source)) {
		UpdateRange(source);
	} else {
		status->setText(QTStr("Studio.ClipLibraryHint"));
	}
}

void StudioClipsDock::Shutdown()
{
	closing = true;
	timer->stop();
}
