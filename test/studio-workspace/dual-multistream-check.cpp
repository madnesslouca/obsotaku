/* Opt-in local RTMP proof: both primary orientations, separate bitrates,
 * shared persistent portrait canvas, audible audio and secondary recovery.
 * The fixture never reads or writes public account/stream-key storage. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <utility/StudioCanvasWorkspace.hpp>
#include <util/config-file.h>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	error = "The local dual-canvas fixture cannot persist destinations";
	return false;
}
namespace {
QString root, server;
QMainWindow *window;
QJsonArray checks, sessions;
QByteArray layout;
OBSSourceAutoRelease original, media;
OBSSceneAutoRelease fixture;
OBSScene portraitScene;
obs_sceneitem_t *marker = nullptr;
OBSCanvas portraitCanvas;
OBSEncoderAutoRelease video, audio;
OBSServiceAutoRelease service;
OBSOutputAutoRelease primary;
std::unique_ptr<MultiStreamManager> manager;
QProcess *receivers[2] = {};
int ports[2] = {}, generation[2] = {}, session = 0;
uint64_t primaryBytes = 0;
bool done = false, studioMode = false, hadMode = false, originalMode = true;
struct Saved {
	const char *section, *key;
	bool present;
	QString value;
};
std::vector<Saved> saved;
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}, {"session", session}});
	if (!pass) {
		throw std::runtime_error(name);
	}
}
void finish(QString error = {});
void later(int ms, std::function<void()> fn)
{
	QTimer::singleShot(ms, qApp, [fn]() {
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
	auto *w = parent->findChild<T *>(name);
	check(w != nullptr, name);
	return w;
}
void edit(std::function<void(QDialog *)> fn, bool apply = true)
{
	later(300, [fn, apply]() {
		auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
		check(d && d->objectName() == "OBSBasicSettings", "native Settings opens");
		child<QListWidget>(d, "listWidget")->setCurrentRow(0);
		fn(d);
		if (apply) {
			child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply)->click();
		}
		child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
	});
	child<QAction>(window, "action_Settings")->trigger();
}
OBSCanvas vertical()
{
	OBSCanvas result;
	obs_frontend_canvas_list list{};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; ++i) {
		if (QString::fromUtf8(obs_canvas_get_name(list.canvases.array[i])) == "Vertical 9:16") {
			result = list.canvases.array[i];
		}
	}
	obs_frontend_canvas_list_free(&list);
	return result;
}
void writeReport(QString error)
{
	QFile f(root + "/runtime.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"sessions", sessions},
					{"scope",
					 "Real primary RTMP output + production MultiStreamManager secondary; main/portrait primary variants, 6200/2800 kbps H.264 and AAC; persistent vertical scene and native UI; two loopback-only MediaMTX receivers. Native Windows execution pending."}})
				.toJson());
	}
}
void restore()
{
	auto *profile = obs_frontend_get_profile_config();
	for (const auto &v : saved) {
		if (v.present) {
			config_set_string(profile, v.section, v.key, v.value.toUtf8().constData());
		} else {
			config_remove_value(profile, v.section, v.key);
		}
	}
	auto *user = obs_frontend_get_user_config();
	if (hadMode) {
		config_set_bool(user, "BasicWindow", "StudioMultiCanvas", originalMode);
	} else {
		config_remove_value(user, "BasicWindow", "StudioMultiCanvas");
	}
	config_save_safe(profile, "tmp", nullptr);
	config_save_safe(user, "tmp", nullptr);
}
void finish(QString error)
{
	if (done) {
		return;
	}
	done = true;
	if (auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
		d->reject();
	}
	if (manager) {
		manager->Stop(true);
	}
	if (primary && obs_output_active(primary)) {
		obs_output_force_stop(primary);
	}
	QTimer::singleShot(1700, qApp, [error]() {
		manager.reset();
		primary = nullptr;
		service = nullptr;
		video = nullptr;
		audio = nullptr;
		MultiStreamManager::SetPortraitCanvasProvider({});
		for (auto *p : receivers) {
			if (p && p->state() != QProcess::NotRunning) {
				p->terminate();
				if (!p->waitForFinished(1500)) {
					p->kill();
					p->waitForFinished(1500);
				}
			}
		}
		if (marker) {
			obs_sceneitem_remove(marker);
			marker = nullptr;
		}
		portraitScene = nullptr;
		portraitCanvas = nullptr;
		if (original) {
			obs_frontend_set_current_scene(original);
		}
		if (fixture) {
			obs_source_remove(obs_scene_get_source(fixture));
		}
		fixture = nullptr;
		if (media) {
			obs_source_remove(media);
		}
		media = nullptr;
		if (studioMode) {
			obs_frontend_set_preview_program_mode(true);
		}
		restore();
		window->restoreState(layout);
		SetStudioPortraitPreview(window, originalMode);
		obs_frontend_reset_video();
		QTimer::singleShot(700, qApp, [error]() {
			obs_frontend_save();
			writeReport(error);
		});
	});
}
void listen(int index)
{
	const QString label = QString("%1-%2-%3").arg(session).arg(index).arg(++generation[index]);
	QString destination = QDir::fromNativeSeparators(
		root + QString("/session-%1/%2/%path/%Y-%m-%d_%H-%M-%S-%f").arg(session).arg(index ? "b" : "a"));
	destination.replace("'", "''");
	QFile config(root + "/receiver-" + label + ".yml");
	check(config.open(QIODevice::WriteOnly), "local receiver configuration is writable");
	config.write(
		QString("logLevel: info\nrtsp: false\nrtmp: true\nrtmpAddress: 127.0.0.1:%1\nhls: false\nwebrtc: false\nsrt: false\nmoq: false\napi: false\nmetrics: false\npprof: false\nplayback: false\npathDefaults:\n  record: true\n  recordPath: '%2'\n  recordFormat: fmp4\n  recordPartDuration: 100ms\n  recordSegmentDuration: 1h\n  recordDeleteAfter: 0s\npaths:\n  all_others: {}\n")
			.arg(ports[index])
			.arg(destination)
			.toUtf8());
	config.close();
	receivers[index]->setStandardOutputFile(root + "/receiver-" + label + ".log");
	receivers[index]->start(server, {config.fileName()});
	check(receivers[index]->waitForStarted(2000), "loopback MediaMTX process starts");
}
OBSOutputAutoRelease secondary()
{
	if (manager) {
		for (auto &entry : manager->ChannelOutputs()) {
			if (entry.id == "dual-local-b") {
				return std::move(entry.output);
			}
		}
	}
	return {};
}
void checkDelivery(bool both = true)
{
	check(primary && obs_output_active(primary), "primary destination stays active");
	check(obs_output_get_total_bytes(primary) > primaryBytes, "primary destination continues delivering bytes");
	primaryBytes = obs_output_get_total_bytes(primary);
	if (both) {
		auto extra = secondary();
		check(extra && obs_output_active(extra) && obs_output_get_total_bytes(extra) > 10000,
		      "secondary destination receives real media");
	}
}
void beginSession();
void waitIdle(int n = 0)
{
	later(100, [n]() {
		auto extra = secondary();
		if (obs_output_active(primary) || (extra && obs_output_active(extra)) || obs_video_active()) {
			check(n < 100, "both RTMP outputs deactivate within deadline");
			waitIdle(n + 1);
			return;
		}
		manager.reset();
		primary = nullptr;
		service = nullptr;
		video = nullptr;
		audio = nullptr;
		for (auto *p : receivers) {
			p->terminate();
			if (!p->waitForFinished(2000)) {
				p->kill();
				p->waitForFinished(2000);
			}
		}
		if (++session == 2) {
			finish();
		} else {
			listen(0);
			listen(1);
			later(800, beginSession);
		}
	});
}
void stopSession()
{
	checkDelivery();
	sessions.append(QJsonObject{{"session", session},
				    {"primary_canvas", session ? "portrait" : "main"},
				    {"secondary_canvas", session ? "main" : "portrait"},
				    {"main_bitrate_kbps", 6200},
				    {"portrait_bitrate_kbps", 2800},
				    {"audio_bitrate_kbps", 160},
				    {"primary_bytes", double(primaryBytes)}});
	manager->Stop();
	obs_output_stop(primary);
	waitIdle();
}
void beginSession()
{
	std::string error;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "bitrate",
			 config_get_int(obs_frontend_get_profile_config(), "SimpleOutput", "VBitrate"));
	obs_data_set_int(settings, "keyint_sec", 2);
	const bool nvenc = obs_get_encoder_codec("obs_nvenc_h264_tex") != nullptr;
	obs_data_set_string(settings, "preset", nvenc ? "p4" : "ultrafast");
	video = obs_video_encoder_create(nvenc ? "obs_nvenc_h264_tex" : "obs_x264", "dual-local-main-video", settings,
					 nullptr);
	obs_encoder_set_video(video, obs_get_video());
	OBSDataAutoRelease a = obs_data_create();
	obs_data_set_int(a, "bitrate", 160);
	audio = obs_audio_encoder_create("ffmpeg_aac", "dual-local-audio", a, 0, nullptr);
	check(bool(video) && bool(audio), "native H.264/AAC stream encoders created");
	obs_encoder_set_audio(audio, obs_get_audio());
	MultiStreamChannel first;
	first.id = "dual-local-a";
	first.displayName = "Local primary";
	first.server = QString("rtmp://127.0.0.1:%1/live").arg(ports[0]).toStdString();
	first.streamKey = "test";
	first.videoLayout = session ? MultiStreamVideoLayout::Portrait : MultiStreamVideoLayout::Main;
	MultiStreamChannel second = first;
	second.id = "dual-local-b";
	second.displayName = "Local secondary";
	second.server = QString("rtmp://127.0.0.1:%1/live").arg(ports[1]).toStdString();
	second.videoLayout = session ? MultiStreamVideoLayout::Main : MultiStreamVideoLayout::Portrait;
	manager = std::make_unique<MultiStreamManager>();
	check(manager->Configure({first, second}, error),
	      "production manager accepts horizontal and vertical destinations");
	manager->SetPrimaryChannelId(first.id);
	const int portraitBitrate =
		int(config_get_int(obs_frontend_get_profile_config(), "StudioCanvas", "PortraitBitrate"));
	auto *firstVideo = manager->PreparePrimaryVideoEncoder(video, error, portraitBitrate);
	check(firstVideo != nullptr, "primary video canvas prepares before transport starts");
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_string(s, "server", first.server.c_str());
	obs_data_set_string(s, "key", "test");
	service = obs_service_create("rtmp_custom", "dual-local-primary-service", s, nullptr);
	primary = obs_output_create("rtmp_output", "dual-local-primary-output", nullptr, nullptr);
	obs_output_set_video_encoder(primary, firstVideo);
	obs_output_set_audio_encoder(primary, audio, 0);
	obs_output_set_service(primary, service);
	check(obs_output_start(primary), "native primary RTMP transport starts");
	MultiStreamOutputSettings options;
	options.portraitBitrateKbps = portraitBitrate;
	options.reconnect = {10, 1};
	options.bindIp = "127.0.0.1";
	check(manager->Start(video, audio, options, error), "production secondary RTMP transport starts");
	auto extra = secondary();
	check(bool(extra), "production manager exposes its secondary output");
	obs_encoder_t *portrait = session ? firstVideo : obs_output_get_video_encoder(extra);
	check(obs_encoder_video(portrait) == obs_canvas_get_video(portraitCanvas),
	      "portrait stream encodes the persistent edited vertical canvas");
	OBSDataAutoRelease mainSettings = obs_encoder_get_settings(video),
			   portraitSettings = obs_encoder_get_settings(portrait);
	check(obs_data_get_int(mainSettings, "bitrate") == 6200 &&
		      obs_data_get_int(portraitSettings, "bitrate") == 2800,
	      "actual simultaneous encoders use separate configured bitrates");
	check(obs_output_get_audio_encoder(primary, 0) == audio && obs_output_get_audio_encoder(extra, 0) == audio,
	      "both destinations carry the selected audio mix");
	primaryBytes = 0;
	later(2500, []() { checkDelivery(); });
	if (session == 0) {
		later(3600, []() {
			checkDelivery();
			receivers[1]->kill();
		});
		later(4800, []() {
			checkDelivery(false);
			check(receivers[1]->state() == QProcess::NotRunning, "vertical receiver actually goes down");
			listen(1);
		});
		later(8500, []() {
			checkDelivery();
			bool live = false;
			for (const auto &s : manager->Snapshot()) {
				if (s.id == "dual-local-b") {
					live = s.state == MultiStreamChannelState::Live && s.health.reconnects >= 1;
				}
			}
			check(live, "vertical destination reconnects while the horizontal primary continues");
		});
		later(11500, stopSession);
	} else {
		later(8500, stopSession);
	}
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "OBS is idle before local dual-canvas multistream probe");
	layout = window->saveState();
	auto *user = obs_frontend_get_user_config();
	hadMode = config_has_user_value(user, "BasicWindow", "StudioMultiCanvas");
	originalMode = !hadMode || config_get_bool(user, "BasicWindow", "StudioMultiCanvas");
	auto *profile = obs_frontend_get_profile_config();
	for (const auto &v : {std::pair{"SimpleOutput", "VBitrate"}, std::pair{"StudioCanvas", "PortraitBitrate"}}) {
		saved.push_back({v.first, v.second, config_has_user_value(profile, v.first, v.second),
				 QString::fromUtf8(config_get_string(profile, v.first, v.second))});
	}
	auto *dock = child<QDockWidget>(window, "verticalPreviewDock");
	dock->hide();
	edit([](QDialog *d) { child<QPushButton>(d, "studioCanvasSingle")->click(); });
	check(dock->isHidden(), "Main Canvas apply hides the vertical preview");
	child<QAction>(window, "studioOutputsAction")->trigger();
	check(dock->isHidden(), "opening multistream respects Main Canvas mode");
	edit([](QDialog *d) {
		child<QPushButton>(d, "studioCanvasMulti")->click();
		child<QSpinBox>(d, "studioCanvasMainBitrate")->setValue(6200);
		child<QSpinBox>(d, "studioCanvasPortraitBitrate")->setValue(2800);
	});
	check(dock->isVisible(), "Multi Canvas apply automatically reveals the vertical preview");
	check(window->tabifiedDockWidgets(dock).isEmpty(),
	      "vertical preview has its own column instead of hiding behind another tab");
	edit([](QDialog *d) { child<QPushButton>(d, "studioCanvasSingle")->click(); }, false);
	check(!dock->isVisible() && !config_get_bool(user, "BasicWindow", "StudioMultiCanvas"),
	      "Cancel preserves the immediate Main Canvas action");
	edit([](QDialog *d) { child<QPushButton>(d, "studioCanvasMulti")->click(); }, false);
	check(dock->isVisible(), "Multi returns immediately without Apply");
	dock->hide();
	child<QAction>(window, "studioOutputsAction")->trigger();
	check(dock->isVisible(), "opening multistream brings back the vertical preview");
	original = obs_frontend_get_current_scene();
	studioMode = obs_frontend_preview_program_mode_active();
	if (studioMode) {
		obs_frontend_set_preview_program_mode(false);
	}
	fixture = obs_scene_create("Dual multistream QA — local media");
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "looping", true);
	obs_data_set_string(settings, "local_file", (root + "/fixture.mp4").toUtf8().constData());
	media = obs_source_create("ffmpeg_source", "Dual multistream QA — pattern and tone", settings, nullptr);
	check(bool(fixture) && bool(media), "local moving video and audible test tone created");
	obs_source_set_audio_mixers(media, 1);
	obs_scene_add(fixture, media);
	obs_frontend_set_current_scene(obs_scene_get_source(fixture));
	later(800, []() {
		portraitCanvas = vertical();
		OBSSourceAutoRelease s = obs_canvas_get_channel(portraitCanvas, 0);
		portraitScene = obs_scene_from_source(s);
		check(bool(portraitScene), "persistent portrait scene follows the main test scene");
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "width", 160);
		obs_data_set_int(settings, "height", 160);
		obs_data_set_int(settings, "color", 0xff33bb55);
		OBSSourceAutoRelease green =
			obs_source_create_private("color_source_v3", "dual-portrait-only-marker", settings);
		marker = obs_scene_add(portraitScene, green);
		vec2 pos{40.f, 40.f};
		obs_sceneitem_set_pos(marker, &pos);
		OBSWeakCanvas weak = OBSGetWeakRef(portraitCanvas);
		MultiStreamManager::SetPortraitCanvasProvider([weak]() { return OBSGetStrongRef(weak); });
		for (int i = 0; i < 2; ++i) {
			QTcpServer freePort;
			check(freePort.listen(QHostAddress::LocalHost, 0),
			      "a private loopback receiver port is available");
			ports[i] = freePort.serverPort();
			freePort.close();
			receivers[i] = new QProcess(qApp);
			receivers[i]->setProcessChannelMode(QProcess::MergedChannels);
			listen(i);
		}
		later(800, []() {
			window->grab().save(root + "/dual-canvas-ui.png");
			beginSession();
		});
	});
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1600, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_DUAL_MULTISTREAM_TEST_ROOT");
	server = qEnvironmentVariable("OBS_STUDIO_DUAL_MULTISTREAM_SERVER");
	if (root.isEmpty() || server.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	later(90000, []() { finish("Dual multistream test exceeded its deadline"); });
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
