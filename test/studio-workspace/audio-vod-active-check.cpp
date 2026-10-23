/* Opt-in targeted native regression: Audio Dock Undo must touch only edited
 * fields/bits; an active RTMP archive freezes its actual encoder mixer despite
 * pending profile changes. Loopback only, no physical input or account store. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTcpServer>
#include <QTimer>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &e)
{
	e = "Active VOD fixture cannot persist destinations";
	return false;
}
namespace {
#if defined(Q_OS_WIN)
constexpr const char *runtimePlatform = "Windows";
constexpr bool runtimeWindows = true, runtimeLinux = false;
#elif defined(Q_OS_LINUX)
constexpr const char *runtimePlatform = "Linux";
constexpr bool runtimeWindows = false, runtimeLinux = true;
#else
constexpr const char *runtimePlatform = "Other";
constexpr bool runtimeWindows = false, runtimeLinux = false;
#endif
QString root, configRoot, server, originalProfile, qaProfile;
QMainWindow *window = nullptr;
QJsonArray checks;
QJsonObject facts;
bool done = false, ready = false, duplicated = false, studioMode = false, globalsCaptured = false;
OBSSceneAutoRelease scene;
OBSSourceAutoRelease source, originalScene, background;
OBSService originalService;
std::array<OBSSource, 5> globals;
std::vector<MultiStreamChannel> originalChannels;
QProcess *receiver = nullptr;
QTimer *silenceTimer = nullptr;
int port = 0;
QByteArray layout;
struct Saved {
	const char *section, *key;
	bool exists;
	QString value;
};
std::vector<Saved> userValues;
void finish(QString error = {});
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name},
				  {"passed", pass},
				  {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
	QSaveFile f(root + "/progress.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"checks", checks}, {"last_check", name}}).toJson());
		f.commit();
	}
	if (!pass) {
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
template<class T> T *child(QObject *p, const char *n)
{
	auto *c = p->findChild<T *>(n);
	check(c != nullptr, n);
	return c;
}
MultiStreamManager *manager()
{
	return static_cast<MultiStreamManager *>(child<QObject>(window, "studioMultistreamManager"));
}
QString profile()
{
	char *p = obs_frontend_get_current_profile();
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}
void report(QString error)
{
	QSaveFile f(root + "/runtime.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"facts", facts},
					{"platform", runtimePlatform},
					{"native_linux", runtimeLinux},
					{"native_windows", runtimeWindows},
					{"scope",
					 "Native Qt Audio Dock Undo/Redo with external source changes; configured destinations in memory; actual loopback RTMP output/archive encoder slots and pending profile changes. No physical input, public ingestion, SRT runtime or restart persistence certification."}})
				.toJson());
		f.commit();
	}
}
void restore(QString error, std::shared_ptr<QElapsedTimer> clock)
{
	if (!ready) {
		report(error);
		return;
	}
	OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
	if ((out && obs_output_active(out)) || obs_frontend_streaming_active() || manager()->IsActive()) {
		if (clock->elapsed() > 15000) {
			report(error + "; output stop deadline, private fixture retained");
			return;
		}
		QTimer::singleShot(100, qApp, [error, clock] { restore(error, clock); });
		return;
	}
	if (receiver) {
		receiver->terminate();
		if (!receiver->waitForFinished(1500)) {
			receiver->kill();
			receiver->waitForFinished(1500);
		}
		receiver->deleteLater();
		receiver = nullptr;
	}
	if (originalScene) {
		obs_frontend_set_current_scene(originalScene);
	}
	if (globalsCaptured) {
		for (size_t i = 0; i < globals.size(); ++i) {
			obs_set_output_source(unsigned(i + 1), globals[i]);
		}
	}
	if (background) {
		obs_source_remove(background);
	}
	if (source) {
		obs_source_remove(source);
	}
	if (scene) {
		obs_source_remove(obs_scene_get_source(scene));
	}
	background = nullptr;
	source = nullptr;
	scene = nullptr;
	std::string e;
	manager()->Configure(originalChannels, e);
	obs_frontend_set_streaming_service(originalService);
	if (studioMode) {
		obs_frontend_set_preview_program_mode(true);
	}
	window->restoreState(layout);
	if (duplicated) {
		obs_frontend_set_current_profile(originalProfile.toUtf8().constData());
		obs_frontend_delete_profile(qaProfile.toUtf8().constData());
		duplicated = false;
	}
	auto *u = obs_frontend_get_user_config();
	for (const auto &v : userValues) {
		if (v.exists) {
			config_set_string(u, v.section, v.key, v.value.toUtf8().constData());
		} else {
			config_remove_value(u, v.section, v.key);
		}
	}
	config_save_safe(u, "tmp", nullptr);
	obs_frontend_save();
	try {
		check(profile() == originalProfile, "original private profile restored after active VOD regression");
		check(!obs_video_active(), "all owned streaming encoders are inactive before handoff");
	} catch (const std::exception &e2) {
		if (error.isEmpty()) {
			error = QString::fromUtf8(e2.what());
		}
	}
	/* The frontend owns the restored objects now. Module globals must not
	 * release stale libobs refs from process-exit destructors after shutdown. */
	for (auto &saved : globals)
		saved = nullptr;
	originalScene = nullptr;
	originalService = nullptr;
	report(error);
}
void finish(QString error)
{
	if (done) {
		return;
	}
	done = true;
	if (silenceTimer) {
		silenceTimer->stop();
	}
	if (auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
		if (d->objectName() == "OBSBasicSettings") {
			if (auto *b = d->findChild<QDialogButtonBox *>("buttonBox")) {
				b->button(QDialogButtonBox::Cancel)->click();
			}
		}
	}
	if (obs_frontend_streaming_active()) {
		obs_frontend_streaming_stop();
	} else {
		OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
		if (out && obs_output_active(out)) {
			obs_output_stop(out);
		}
	}
	auto clock = std::make_shared<QElapsedTimer>();
	clock->start();
	QTimer::singleShot(100, qApp, [error, clock] { restore(error, clock); });
}
void action(const char *n)
{
	auto *a = child<QAction>(window, n);
	check(a->isEnabled(), n);
	a->trigger();
}
void refresh()
{
	check(QMetaObject::invokeMethod(window, "OpenStudioAudioRouting", Qt::DirectConnection,
					Q_ARG(QString, QString::fromUtf8(obs_source_get_uuid(source)))),
	      "native audio dock focuses actual fixture source");
}
void recordPlan(const char *label)
{
	auto *b = child<QPushButton>(window, "studioAudioVodExclude");
	facts[label] = QJsonObject{{"live_mask", int(b->property("studioLiveMask").toUInt())},
				   {"vod_mask", int(b->property("studioVodMask").toUInt())},
				   {"effective", b->property("studioVodEffective").toBool()},
				   {"active", b->property("studioVodActiveSession").toBool()}};
}
void activeChecks()
{
	OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
	check(out && obs_output_active(out), "native RTMP output remains active during pending preference checks");
	auto *a = obs_output_get_audio_encoder(out, 0), *v = obs_output_get_audio_encoder(out, 1);
	check(a && v && obs_encoder_get_mixer_index(a) == 0 && obs_encoder_get_mixer_index(v) == 1,
	      "actual RTMP program slot0 and archive slot1 use distinct intended mixers");
	auto *b = child<QPushButton>(window, "studioAudioVodExclude");
	check(b->property("studioVodActiveSession").toBool() && b->property("studioVodEffective").toBool(),
	      "VOD interface marks the actual live archive as effective");
	check(b->property("studioVodMask").toUInt() == 2 && b->property("studioLiveMask").toUInt() == 1,
	      "active UI targets the actual archive and protects the actual program mix");
	recordPlan("active_initial");
	config_set_int(obs_frontend_get_profile_config(), "AdvOut", "VodTrackIndex", 4);
	refresh();
	later(700, [] {
		OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
		auto *v = obs_output_get_audio_encoder(out, 1);
		check(v && obs_encoder_get_mixer_index(v) == 1,
		      "pending VOD preference cannot mutate active encoder mixer");
		auto *b = child<QPushButton>(window, "studioAudioVodExclude");
		check(b->property("studioVodMask").toUInt() == 2 && b->property("studioVodEffective").toBool(),
		      "pending VOD index4 leaves active archive target frozen to actual mixer2");
		b->click();
		check(obs_source_get_audio_mixers(source) == 5,
		      "active VOD toggle excludes only effective archive bit preserving live and record");
		action("actionMainUndo");
		check(obs_source_get_audio_mixers(source) == 7,
		      "Undo restores effective VOD bit while preferences differ");
		recordPlan("active_with_pending_pref");
		facts["active_stream_bytes"] = qint64(obs_output_get_total_bytes(out));
		finish();
	});
}
void waitActive(std::shared_ptr<QElapsedTimer> clock)
{
	OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
	if (!out || !obs_output_active(out) || obs_output_get_total_bytes(out) < 4000) {
		check(clock->elapsed() < 15000, "native RTMP reaches active delivery before deadline");
		later(100, [clock] { waitActive(clock); });
		return;
	}
	later(700, activeChecks);
}
void goLive()
{
	silenceTimer = new QTimer(qApp);
	silenceTimer->setInterval(20);
	QObject::connect(silenceTimer, &QTimer::timeout, qApp, [] {
		float samples[960] = {};
		obs_source_audio audio{};
		audio.frames = 960;
		audio.samples_per_sec = 48000;
		audio.speakers = SPEAKERS_MONO;
		audio.format = AUDIO_FORMAT_FLOAT;
		audio.timestamp = os_gettime_ns();
		audio.data[0] = reinterpret_cast<uint8_t *>(samples);
		obs_source_output_audio(source, &audio);
	});
	silenceTimer->start();
	auto *c = obs_frontend_get_profile_config();
	config_set_bool(c, "AdvOut", "VodTrackEnabled", true);
	config_set_int(c, "AdvOut", "TrackIndex", 1);
	config_set_int(c, "AdvOut", "VodTrackIndex", 2);
	MultiStreamChannel channel;
	channel.id = "audio-active-vod-qa";
	channel.displayName = "Local VOD QA";
	channel.platform = StreamPlatform::Twitch;
	channel.server = QString("rtmp://127.0.0.1:%1/live").arg(port).toStdString();
	channel.streamKey = "test";
	channel.audioMixIndex = 0;
	channel.vodTrackEnabled = true;
	channel.vodTrackIndex = 1;
	std::string e;
	check(manager()->Configure({channel}, e),
	      "native primary destination is an explicit loopback RTMP test with archive");
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_string(d, "service", "Twitch");
	obs_data_set_string(d, "server", channel.server.c_str());
	obs_data_set_string(d, "key", "test");
	OBSServiceAutoRelease service = obs_service_create("rtmp_common", "audio-active-loopback", d, nullptr);
	obs_frontend_set_streaming_service(service);
	check(QMetaObject::invokeMethod(window, "StreamActionTriggered", Qt::DirectConnection),
	      "real native Go Live action starts the loopback-only session");
	auto clock = std::make_shared<QElapsedTimer>();
	clock->start();
	later(100, [clock] { waitActive(clock); });
}
void receiverReady()
{
	QTcpServer tcp;
	if (!tcp.listen(QHostAddress::LocalHost, 0)) {
		throw std::runtime_error("cannot allocate loopback receiver port");
	}
	port = tcp.serverPort();
	tcp.close();
	QFile f(root + "/mediamtx.yml");
	check(f.open(QIODevice::WriteOnly), "private receiver config writable");
	f.write(QString("logLevel: warn\nrtsp: no\nrtmp: yes\nrtmpAddress: 127.0.0.1:%1\nhls: no\nwebrtc: no\nsrt: no\nmoq: no\nplayback: no\napi: no\nmetrics: no\npprof: no\npaths:\n  all_others:\n    source: publisher\n")
			.arg(port)
			.toUtf8());
	f.close();
	receiver = new QProcess(qApp);
	receiver->setProgram(server);
	receiver->setWorkingDirectory(root);
	receiver->setArguments({f.fileName()});
	receiver->setStandardOutputFile(root + "/receiver.log");
	receiver->setStandardErrorFile(root + "/receiver.log", QIODevice::Append);
	receiver->start();
	later(700, [] {
		check(receiver->state() == QProcess::Running, "loopback receiver process starts");
		goLive();
	});
}
void memoryPlan()
{
	auto *u = obs_frontend_get_user_config();
	uint32_t stored = 0;
	int count = int(config_get_int(u, "MultistreamChannels", "Count"));
	bool found = false;
	for (int i = 0; i < std::min(count, 32); ++i) {
		const auto p = QString("Channel%1.").arg(i);
		found |= QString::fromUtf8(config_get_string(u, "MultistreamChannels",
							     (p + "Id").toUtf8().constData())) == "audio-active-vod-qa";
		if (config_get_bool(u, "MultistreamChannels", (p + "VodTrackEnabled").toUtf8().constData())) {
			auto v = config_get_int(u, "MultistreamChannels", (p + "VodTrackIndex").toUtf8().constData());
			if (v >= 0 && v < 6) {
				stored |= 1u << v;
			}
		}
	}
	check(!found, "reserved QA channel is absent from persistent account metadata");
	const int mix = stored == 8 ? 4 : 3;
	auto *c = obs_frontend_get_profile_config();
	config_set_bool(c, "AdvOut", "VodTrackEnabled", false);
	MultiStreamChannel channel;
	channel.id = "audio-active-vod-qa";
	channel.displayName = "Memory-only clean mix";
	channel.platform = StreamPlatform::Twitch;
	channel.server = "rtmp://127.0.0.1:9/live";
	channel.streamKey = "test";
	channel.audioMixIndex = 0;
	channel.vodTrackEnabled = true;
	channel.vodTrackIndex = mix;
	std::string e;
	check(manager()->Configure({channel}, e),
	      "QA configures transient destinations without saving any account metadata");
	refresh();
	later(600, [mix, stored, count] {
		auto *b = child<QPushButton>(window, "studioAudioVodExclude");
		check(b->property("studioVodMask").toUInt() == (1u << mix),
		      "prepared VOD UI follows in-memory configured destination rather than persistent account store");
		check(!b->property("studioVodEffective").toBool(),
		      "prepared clean mix is never labeled an active archive");
		facts["stored_destination_count"] = count;
		facts["stored_vod_mask"] = int(stored);
		recordPlan("memory_only_prepared");
		receiverReady();
	});
}
void undoChecks()
{
	auto *gain = child<QDoubleSpinBox>(window, "studioAudioGain");
	gain->setValue(-9);
	check(QMetaObject::invokeMethod(gain, "editingFinished", Qt::DirectConnection),
	      "native gain edit is committed");
	check(std::abs(obs_mul_to_db(obs_source_get_volume(source)) + 9) < .1, "native gain reaches source");
	obs_source_set_muted(source, true);
	obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_ONLY);
	obs_source_set_audio_mixers(source, 5);
	obs_source_set_sync_offset(source, 17000000);
	action("actionMainUndo");
	check(std::abs(obs_source_get_volume(source) - 1.f) < .001 && obs_source_muted(source) &&
		      obs_source_get_monitoring_type(source) == OBS_MONITORING_TYPE_MONITOR_ONLY &&
		      obs_source_get_audio_mixers(source) == 5 && obs_source_get_sync_offset(source) == 17000000,
	      "gain Undo changes only gain and preserves externally changed mute monitoring mixes and delay");
	action("actionMainRedo");
	check(std::abs(obs_mul_to_db(obs_source_get_volume(source)) + 9) < .1 && obs_source_muted(source) &&
		      obs_source_get_monitoring_type(source) == OBS_MONITORING_TYPE_MONITOR_ONLY &&
		      obs_source_get_audio_mixers(source) == 5,
	      "gain Redo preserves unrelated external edits");
	obs_source_set_volume(source, 1);
	obs_source_set_muted(source, false);
	obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_NONE);
	obs_source_set_audio_mixers(source, 7);
	obs_source_set_sync_offset(source, 0);
	refresh();
	later(500, [] {
		auto *table = child<QTableWidget>(window, "studioAudioRouting");
		int row = table->currentRow();
		check(row >= 0 && table->item(row, 0)->data(Qt::UserRole).toString() ==
					  QString::fromUtf8(obs_source_get_uuid(source)),
		      "routing table targets the QA source");
		table->item(row, 2)->setCheckState(Qt::Unchecked);
		check(obs_source_get_audio_mixers(source) == 5, "native routing edit changes only selected VOD bit");
		obs_source_set_audio_mixers(source, 21);
		obs_source_set_muted(source, true);
		obs_source_set_sync_offset(source, 23000000);
		action("actionMainUndo");
		check(obs_source_get_audio_mixers(source) == 23 && obs_source_muted(source) &&
			      obs_source_get_sync_offset(source) == 23000000,
		      "routing Undo restores only edited bit and preserves unrelated bit plus external state");
		action("actionMainRedo");
		check(obs_source_get_audio_mixers(source) == 21 && obs_source_muted(source) &&
			      obs_source_get_sync_offset(source) == 23000000,
		      "routing Redo preserves unrelated external bits and fields");
		obs_source_set_audio_mixers(source, 7);
		obs_source_set_muted(source, false);
		obs_source_set_sync_offset(source, 0);
		memoryPlan();
	});
}
void setup()
{
	check(profile() == qaProfile, "targeted regression uses dedicated duplicate profile");
	auto *c = obs_frontend_get_profile_config();
	config_set_int(c, "AdvOut", "TrackIndex", 1);
	config_set_int(c, "AdvOut", "VodTrackIndex", 2);
	config_set_bool(c, "AdvOut", "VodTrackEnabled", true);
	config_set_string(c, "AdvOut", "Encoder", "obs_x264");
	config_set_string(c, "AdvOut", "AudioEncoder", "ffmpeg_aac");
	config_set_int(c, "AdvOut", "StreamMultiTrackAudioMixes", 1);
	config_set_bool(c, "AdvOut", "StreamMultiTrackAudio", false);
	for (int i = 1; i <= 6; ++i) {
		config_set_int(c, "AdvOut", QString("Track%1Bitrate").arg(i).toUtf8().constData(), 160);
	}
	config_set_string(c, "StudioCanvas", "MainStreamResolution", "640x360");
	config_set_bool(c, "Stream1", "EnableMultitrackVideo", false);
	config_set_bool(c, "Output", "DelayEnable", false);
	char *p = obs_frontend_get_current_profile_path();
	QFile enc(QString::fromUtf8(p) + "/streamEncoder.json");
	bfree(p);
	check(enc.open(QIODevice::WriteOnly), "temporary stream encoder settings writable");
	enc.write("{\"rate_control\":\"CBR\",\"bitrate\":600,\"keyint_sec\":2,\"preset\":\"ultrafast\"}");
	enc.close();
	later(300, [] {
		auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
		check(d && d->objectName() == "OBSBasicSettings", "native Settings opens for Advanced output handler");
		child<QComboBox>(d, "outputMode")->setCurrentIndex(1);
		child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply)->click();
		child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
	});
	child<QAction>(window, "action_Settings")->trigger();
	if (done) {
		return;
	}
	for (size_t i = 0; i < globals.size(); ++i) {
		globals[i] = obs_get_output_source(unsigned(i + 1));
		obs_set_output_source(unsigned(i + 1), nullptr);
	}
	globalsCaptured = true;
	if (studioMode) {
		obs_frontend_set_preview_program_mode(false);
	}
	scene = obs_scene_create("Audio active VOD QA");
	source = obs_source_create("studio_audio_active_qa", "Audio active VOD QA source", nullptr, nullptr);
	check(source && scene, "inert QA source and independent scene created");
	/* A fresh private collection has no other video source. Satisfy the real
	 * NoSources confirmation prerequisite without accepting a modal for it. */
	OBSDataAutoRelease color = obs_data_create();
	obs_data_set_int(color, "width", 640);
	obs_data_set_int(color, "height", 360);
	obs_data_set_int(color, "color", 0xff202020);
	background = obs_source_create("color_source_v3", "Audio active VOD QA private video", color, nullptr);
	check(background && (obs_source_get_output_flags(background) & OBS_SOURCE_VIDEO),
	      "private synthetic640x360 video satisfies native no-video-source validation");
	obs_scene_add(scene, background);
	obs_source_set_audio_mixers(source, 7);
	obs_scene_add(scene, source);
	obs_frontend_set_current_scene(obs_scene_get_source(scene));
	refresh();
	later(600, undoChecks);
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "OBS starts idle for targeted active VOD test");
	const auto expected = QFileInfo(configRoot).canonicalFilePath();
	check(!configRoot.isEmpty() && !expected.isEmpty(), "explicit private configuration root exists");
	char *p = obs_frontend_get_current_profile_path();
	auto path = QFileInfo(QString::fromUtf8(p)).canonicalFilePath();
	bfree(p);
	check(path.startsWith(expected + "/"), "effective profile is in private root before any mutation");
	for (const char *k : {"Configuration", "SceneCollections", "Profiles", "PluginManagerSettings"}) {
		check(QFileInfo(QString::fromUtf8(config_get_string(obs_frontend_get_app_config(), "Locations", k)))
				      .canonicalFilePath() == expected,
		      "all four absolute Locations point at private root");
	}
	layout = window->saveState();
	originalProfile = profile();
	originalScene = obs_frontend_get_current_scene();
	originalService = obs_frontend_get_streaming_service();
	originalChannels = manager()->ConfiguredChannels();
	studioMode = obs_frontend_preview_program_mode_active();
	ready = true;
	auto *u = obs_frontend_get_user_config();
	for (const char *k : {"WarnBeforeStartingStream", "WarnBeforeStoppingStream", "RecordWhenStreaming",
			      "ReplayBufferWhileStreaming"}) {
		userValues.push_back({"BasicWindow", k, config_has_user_value(u, "BasicWindow", k),
				      QString::fromUtf8(config_get_string(u, "BasicWindow", k))});
		config_set_bool(u, "BasicWindow", k, false);
	}
	userValues.push_back({"General", "EnableCustomServerVodTrack",
			      config_has_user_value(u, "General", "EnableCustomServerVodTrack"),
			      QString::fromUtf8(config_get_string(u, "General", "EnableCustomServerVodTrack"))});
	config_set_bool(u, "General", "EnableCustomServerVodTrack", true);
	qaProfile = "Audio active VOD QA " + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
	obs_frontend_duplicate_profile(qaProfile.toUtf8().constData());
	duplicated = true;
	later(600, setup);
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1500, run);
	} else if (e == OBS_FRONTEND_EVENT_EXIT) {
		for (auto &saved : globals)
			saved = nullptr;
		originalScene = nullptr;
		originalService = nullptr;
	}
}
const char *name(void *)
{
	return "Audio active QA input";
}
void *create(obs_data_t *, obs_source_t *s)
{
	return s;
}
void destroy(void *) {}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_ACTIVE_VOD_ROOT");
	configRoot = qEnvironmentVariable("OBS_STUDIO_ACTIVE_VOD_CONFIG_ROOT");
	server = qEnvironmentVariable("OBS_STUDIO_ACTIVE_VOD_SERVER");
	if (root.isEmpty() || server.isEmpty() || QFileInfo::exists(root + "/runtime.json") || !QDir().mkpath(root)) {
		return false;
	}
	obs_source_info i{};
	i.id = "studio_audio_active_qa";
	i.type = OBS_SOURCE_TYPE_INPUT;
	i.output_flags = OBS_SOURCE_AUDIO;
	i.get_name = name;
	i.create = create;
	i.destroy = destroy;
	obs_register_source(&i);
	obs_frontend_add_event_callback(event, nullptr);
	QTimer::singleShot(45000, qApp, [] {
		if (!done) {
			finish("targeted VOD fixture exceeded 45 second deadline");
		}
	});
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
