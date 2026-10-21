/* Opt-in native frontend integration: the real app's stream manager, Record,
 * Replay Save and Stop/Pause APIs run together against loopback RTMP receivers.
 * A temporary duplicate profile keeps output/encoder settings isolated. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <util/config-file.h>
#include <util/bmem.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <stdexcept>
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	error = "The simultaneous fixture cannot persist destinations";
	return false;
}
namespace {
QString root, server, originalProfile, qaProfile;
QMainWindow *window;
QJsonArray checks, files;
QProcess *receivers[2] = {};
int ports[2] = {}, generation[2] = {}, phase = 0;
OBSSourceAutoRelease original, media;
OBSSceneAutoRelease fixture;
OBSScene portraitScene;
OBSCanvas portraitCanvas;
OBSService originalService;
obs_sceneitem_t *marker = nullptr;
std::vector<MultiStreamChannel> originalChannels;
bool done = false, studioMode = false, duplicated = false;
uint64_t bytesBefore[3] = {};
struct UserValue {
	const char *key;
	bool exists;
	QString value;
};
std::vector<UserValue> userValues;
void finish(QString error = {});
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}, {"phase", phase}});
	if (!pass) {
		throw std::runtime_error(name);
	}
}
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
template<class T> T *child(QObject *p, const char *name)
{
	auto *w = p->findChild<T *>(name);
	check(w != nullptr, name);
	return w;
}
MultiStreamManager *manager()
{
	auto *object = window->findChild<QObject *>("studioMultistreamManager");
	check(object != nullptr, "the real native output handler exposes its current manager");
	return static_cast<MultiStreamManager *>(object);
}
OBSOutputAutoRelease secondary()
{
	for (auto &entry : manager()->ChannelOutputs()) {
		if (entry.id == "simultaneous-local-b") {
			return std::move(entry.output);
		}
	}
	return {};
}
OBSOutputAutoRelease companion()
{
	return obs_get_output_by_name("studio_portrait_recording");
}
void edit(std::function<void(QDialog *)> fn)
{
	later(300, [fn]() {
		auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
		check(d && d->objectName() == "OBSBasicSettings", "native output Settings opens");
		child<QListWidget>(d, "listWidget")->setCurrentRow(0);
		fn(d);
		child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Apply)->click();
		child<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
	});
	child<QAction>(window, "action_Settings")->trigger();
}
void choice(QDialog *d, const char *name, const char *value)
{
	auto *c = child<QComboBox>(d, name);
	int i = c->findData(value);
	check(i >= 0, "required native output choice exists");
	c->setCurrentIndex(i);
}
OBSCanvas vertical()
{
	OBSCanvas c;
	obs_frontend_canvas_list list{};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; ++i) {
		if (QString::fromUtf8(obs_canvas_get_name(list.canvases.array[i])) == "Vertical 9:16") {
			c = list.canvases.array[i];
		}
	}
	obs_frontend_canvas_list_free(&list);
	return c;
}
void report(const QString &error)
{
	QFile f(root + "/runtime.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"files", files},
					{"scope",
					 "Actual native frontend streaming + real owned MultiStreamManager, dual recording, native replay save/library, Simple and Advanced output modes, native main primary and portrait secondary, pause, reverse start/stop order, network recovery; loopback RTMP only. Native Windows execution pending."}})
				.toJson());
	}
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
	if (obs_frontend_recording_active()) {
		obs_frontend_recording_stop();
	}
	if (obs_frontend_replay_buffer_active()) {
		obs_frontend_replay_buffer_stop();
	}
	if (obs_frontend_streaming_active()) {
		obs_frontend_streaming_stop();
	}
	QTimer::singleShot(2200, qApp, [error]() {
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
		original = nullptr;
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
		if (duplicated) {
			obs_frontend_set_current_profile(originalProfile.toUtf8().constData());
		}
		QTimer::singleShot(700, qApp, [error]() {
			if (duplicated) {
				obs_frontend_delete_profile(qaProfile.toUtf8().constData());
			}
			std::string ignored;
			manager()->Configure(originalChannels, ignored);
			if (originalService) {
				obs_frontend_set_streaming_service(originalService);
			}
			originalService = nullptr;
			auto *u = obs_frontend_get_user_config();
			for (const auto &v : userValues) {
				if (v.exists) {
					config_set_string(u, "BasicWindow", v.key, v.value.toUtf8().constData());
				} else {
					config_remove_value(u, "BasicWindow", v.key);
				}
			}
			config_save_safe(u, "tmp", nullptr);
			obs_frontend_reset_video();
			QTimer::singleShot(500, qApp, [error]() {
				obs_frontend_save();
				report(error);
			});
		});
	});
}
void listen(int index)
{
	const QString label = QString("%1-%2-%3").arg(phase).arg(index).arg(++generation[index]);
	QString destination = QDir::fromNativeSeparators(
		root + QString("/phase-%1/%2/%path/%Y-%m-%d_%H-%M-%S-%f").arg(phase).arg(index ? "b" : "a"));
	destination.replace("'", "''");
	QFile c(root + "/receiver-" + label + ".yml");
	check(c.open(QIODevice::WriteOnly), "local receiver configuration writable");
	c.write(QString("logLevel: info\nrtsp: false\nrtmp: true\nrtmpAddress: 127.0.0.1:%1\nhls: false\nwebrtc: false\nsrt: false\nmoq: false\napi: false\nmetrics: false\npprof: false\nplayback: false\npathDefaults:\n  record: true\n  recordPath: '%2'\n  recordFormat: fmp4\n  recordPartDuration: 100ms\n  recordSegmentDuration: 1h\n  recordDeleteAfter: 0s\npaths:\n  all_others: {}\n")
			.arg(ports[index])
			.arg(destination)
			.toUtf8());
	c.close();
	receivers[index]->setStandardOutputFile(root + "/receiver-" + label + ".log");
	receivers[index]->start(server, {c.fileName()});
	check(receivers[index]->waitForStarted(2000), "loopback receiver starts");
}
void allActive()
{
	check(obs_frontend_streaming_active() && obs_frontend_recording_active() && obs_frontend_replay_buffer_active(),
	      "native streaming, recording and replay are active at the same time");
	OBSOutputAutoRelease s = obs_frontend_get_streaming_output(), r = obs_frontend_get_recording_output(),
			     b = obs_frontend_get_replay_buffer_output();
	auto x = companion(), t = secondary();
	check(t && obs_output_active(t) && x && obs_output_active(x),
	      "both streaming destinations and both recording files are active");
	check(obs_output_get_total_bytes(s) > 10000 && obs_output_get_total_bytes(t) > 10000 &&
		      obs_output_get_total_bytes(r) > 10000 && obs_output_get_total_bytes(x) > 10000,
	      "all four delivery outputs produce real bytes");
	check(obs_encoder_get_width(obs_output_get_video_encoder(s)) == 1920u,
	      "native stream primary uses the selected orientation");
	check(obs_encoder_get_width(obs_output_get_video_encoder(t)) == 1080u,
	      "secondary stream uses the other orientation");
	check(obs_encoder_get_width(obs_output_get_video_encoder(r)) == 1920 &&
		      obs_encoder_get_width(obs_output_get_video_encoder(x)) == 1080,
	      "native recording simultaneously writes main and portrait canvases");
	check(obs_encoder_get_width(obs_output_get_video_encoder(b)) == (phase % 2 ? 1080u : 1920u),
	      "native clip buffer uses the chosen canvas independently");
	for (auto *o : {s.Get(), t.Get(), r.Get(), x.Get(), b.Get()}) {
		check(obs_output_get_audio_encoder(o, 0) != nullptr,
		      "every simultaneous output has its selected audio encoder");
	}
}
void saveClip()
{
	obs_frontend_replay_buffer_save();
	later(1100, []() {
		char *p = obs_frontend_get_last_replay();
		QString path = QString::fromUtf8(p);
		bfree(p);
		check(QFileInfo(path).size() > 10000, "native Save Clip writes media while other outputs are active");
		OBSOutputAutoRelease b = obs_frontend_get_replay_buffer_output();
		auto *v = obs_output_get_video_encoder(b);
		files.append(QJsonObject{{"path", path},
					 {"kind", "clip"},
					 {"phase", phase},
					 {"width", int(obs_encoder_get_width(v))},
					 {"height", int(obs_encoder_get_height(v))}});
		auto *library = child<QListWidget>(window, "studioClipLibrary");
		bool found = false;
		for (int i = 0; i < library->count(); ++i) {
			if (library->item(i)->data(Qt::UserRole).toString() == path) {
				found = true;
			}
		}
		check(found, "the native clip library registers each saved clip");
	});
}
void collectRecordings()
{
	char *p = obs_frontend_get_last_recording();
	QString path = QString::fromUtf8(p);
	bfree(p);
	files.append(
		QJsonObject{{"path", path}, {"kind", "recording"}, {"phase", phase}, {"width", 1920}, {"height", 1080}});
	auto x = companion();
	OBSDataAutoRelease d = obs_output_get_settings(x);
	files.append(QJsonObject{{"path", QString::fromUtf8(obs_data_get_string(d, "path"))},
				 {"kind", "companion"},
				 {"phase", phase},
				 {"width", 1080},
				 {"height", 1920}});
}
void phaseRun();
void waitIdle(int n = 0)
{
	later(150, [n]() {
		auto t = secondary(), x = companion();
		if (obs_frontend_streaming_active() || obs_frontend_recording_active() || manager()->IsActive() ||
		    obs_frontend_replay_buffer_active() || obs_video_active() || (t && obs_output_active(t)) ||
		    (x && obs_output_active(x))) {
			check(n < 100, "all simultaneous outputs stop within deadline");
			waitIdle(n + 1);
			return;
		}
		for (auto *p : receivers) {
			p->terminate();
			if (!p->waitForFinished(1500)) {
				p->kill();
				p->waitForFinished(1500);
			}
		}
		if (++phase == 4) {
			finish();
		} else {
			phaseRun();
		}
	});
}
void stopAll()
{
	if (obs_frontend_recording_active()) {
		collectRecordings();
	}
	if (obs_frontend_recording_active()) {
		obs_frontend_recording_stop();
	}
	if (obs_frontend_replay_buffer_active()) {
		obs_frontend_replay_buffer_stop();
	}
	if (obs_frontend_streaming_active()) {
		obs_frontend_streaming_stop();
	}
	waitIdle();
}
void goLive()
{
	check(QMetaObject::invokeMethod(window, "StreamActionTriggered", Qt::DirectConnection),
	      "the native Go Live action is invoked");
}
void exercise()
{
	if (phase == 0) {
		goLive();
		later(1500, [] {
			obs_frontend_recording_start();
			obs_frontend_replay_buffer_start();
		});
	} else if (phase == 1 || phase == 2) {
		obs_frontend_recording_start();
		obs_frontend_replay_buffer_start();
		later(1200, goLive);
	} else {
		obs_frontend_replay_buffer_start();
		later(1000, [] {
			goLive();
			obs_frontend_recording_start();
		});
	}
	later(3500, allActive);
	if (phase == 0) {
		later(4500, [] { obs_frontend_recording_pause(true); });
		later(4900, [] {
			auto x = companion();
			check(obs_frontend_recording_paused() && obs_output_paused(x),
			      "Pause affects both recordings while streams and replay stay active");
			check(obs_frontend_streaming_active() && obs_frontend_replay_buffer_active(),
			      "recording pause leaves live and clip buffer running");
			obs_frontend_recording_pause(false);
		});
		later(6200, saveClip);
		later(7800, [] {
			auto s = OBSOutputAutoRelease{obs_frontend_get_streaming_output()};
			bytesBefore[0] = obs_output_get_total_bytes(s);
			receivers[1]->kill();
		});
		later(9100, [] {
			auto s = OBSOutputAutoRelease{obs_frontend_get_streaming_output()};
			check(obs_output_get_total_bytes(s) > bytesBefore[0],
			      "horizontal primary continues while the vertical receiver is down");
			listen(1);
		});
		later(12800, [] {
			allActive();
			bool live = false;
			for (const auto &s : manager()->Snapshot()) {
				if (s.id == "simultaneous-local-b") {
					live = s.state == MultiStreamChannelState::Live && s.health.reconnects >= 1;
				}
			}
			check(live, "vertical reconnects while recording and clips remain active");
			saveClip();
		});
		later(14600, [] {
			collectRecordings();
			obs_frontend_recording_stop();
			obs_frontend_replay_buffer_stop();
			auto s = OBSOutputAutoRelease{obs_frontend_get_streaming_output()};
			bytesBefore[0] = obs_output_get_total_bytes(s);
		});
		later(16000, [] {
			auto s = OBSOutputAutoRelease{obs_frontend_get_streaming_output()};
			check(obs_frontend_streaming_active() && obs_output_get_total_bytes(s) > bytesBefore[0],
			      "stopping recording and replay leaves both streams live");
			obs_frontend_streaming_stop();
			waitIdle();
		});
	} else if (phase == 1) {
		later(5600, saveClip);
		later(7600, [] {
			OBSOutputAutoRelease r = obs_frontend_get_recording_output();
			bytesBefore[0] = obs_output_get_total_bytes(r);
			obs_frontend_streaming_stop();
		});
		later(9500, [] {
			OBSOutputAutoRelease r = obs_frontend_get_recording_output();
			check(!obs_frontend_streaming_active() && obs_frontend_recording_active() &&
				      obs_frontend_replay_buffer_active() &&
				      obs_output_get_total_bytes(r) > bytesBefore[0],
			      "stopping both streams leaves recording and replay running");
			saveClip();
		});
		later(11600, stopAll);
	} else if (phase == 2) {
		later(5500, saveClip);
		later(8500, saveClip);
		later(11000, stopAll);
	} else {
		later(5600, saveClip);
		later(7400, [] { obs_frontend_replay_buffer_stop(); });
		later(8800, [] {
			check(!obs_frontend_replay_buffer_active() && obs_frontend_streaming_active() &&
				      obs_frontend_recording_active(),
			      "stopping replay leaves native recording and streaming running");
			obs_frontend_replay_buffer_start();
		});
		later(13000, [] {
			allActive();
			saveClip();
		});
		later(15500, stopAll);
	}
}
void phaseRun()
{
	auto *c = obs_frontend_get_profile_config();
	QString dir = root + QString("/phase-%1/local").arg(phase);
	QDir().mkpath(dir);
	config_set_int(c, "SimpleOutput", "VBitrate", 6200);
	config_set_int(c, "SimpleOutput", "ABitrate", 160);
	config_set_bool(c, "SimpleOutput", "UseAdvanced", false);
	config_set_string(c, "SimpleOutput", "StreamEncoder",
			  obs_get_encoder_codec("obs_nvenc_h264_tex") ? "nvenc" : "x264");
	config_set_bool(c, "SimpleOutput", "RecRB", true);
	config_set_int(c, "SimpleOutput", "RecRBTime", 5);
	config_set_string(c, "SimpleOutput", "RecFormat2", "mkv");
	config_set_string(c, "AdvOut", "Encoder",
			  obs_get_encoder_codec("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "obs_x264");
	config_set_string(c, "AdvOut", "RecEncoder", "none");
	config_set_string(c, "AdvOut", "RecType", "Standard");
	config_set_string(c, "AdvOut", "RecFormat2", "mkv");
	config_set_string(c, "AdvOut", "RecFilePath", dir.toUtf8().constData());
	config_set_bool(c, "AdvOut", "RecRB", true);
	config_set_int(c, "AdvOut", "RecRBTime", 5);
	config_set_int(c, "AdvOut", "RecTracks", 1);
	config_set_bool(c, "Stream1", "EnableMultitrackVideo", false);
	config_set_bool(c, "Output", "DelayEnable", false);
	config_set_bool(c, "Output", "Reconnect", true);
	config_set_int(c, "Output", "RetryDelay", 1);
	config_set_int(c, "Output", "MaxRetries", 10);
	char *profilePath = obs_frontend_get_current_profile_path();
	QFile enc(QString::fromUtf8(profilePath) + "/streamEncoder.json");
	bfree(profilePath);
	check(enc.open(QIODevice::WriteOnly), "temporary profile encoder settings writable");
	enc.write(
		QJsonDocument(QJsonObject{{"rate_control", "CBR"},
					  {"bitrate", 6200},
					  {"keyint_sec", 2},
					  {"preset", obs_get_encoder_codec("obs_nvenc_h264_tex") ? "p4" : "ultrafast"}})
			.toJson());
	enc.close();
	edit([dir](QDialog *d) {
		child<QComboBox>(d, "outputMode")->setCurrentIndex(phase >= 2 ? 1 : 0);
		child<QLineEdit>(d, "simpleOutputPath")->setText(dir);
		child<QPushButton>(d, "studioCanvasMulti")->click();
		child<QSpinBox>(d, "studioCanvasPortraitBitrate")->setValue(2800);
		child<QSpinBox>(d, "studioCanvasPortraitRecordingBitrate")->setValue(3600);
		choice(d, "studioCanvasRecordingCanvas", "both");
		choice(d, "studioCanvasReplayCanvas", phase % 2 ? "portrait" : "main");
	});
	if (done) {
		return;
	}
	MultiStreamChannel a;
	a.id = "simultaneous-local-a";
	a.displayName = "Local primary";
	a.server = QString("rtmp://127.0.0.1:%1/live").arg(ports[0]).toStdString();
	a.streamKey = "test";
	a.videoLayout = MultiStreamVideoLayout::Main;
	MultiStreamChannel b = a;
	b.id = "simultaneous-local-b";
	b.displayName = "Local secondary";
	b.server = QString("rtmp://127.0.0.1:%1/live").arg(ports[1]).toStdString();
	b.videoLayout = MultiStreamVideoLayout::Portrait;
	std::string error;
	const bool configured = manager()->Configure({a, b}, error);
	if (!configured) {
		throw std::runtime_error(error);
	}
	check(configured, "the app's real multistream manager gets only two loopback destinations");
	OBSServiceAutoRelease empty =
		obs_service_create("rtmp_common", "simultaneous-empty-before-promotion", nullptr, nullptr);
	obs_frontend_set_streaming_service(empty);
	listen(0);
	listen(1);
	later(900, exercise);
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "native OBS starts idle before combined test");
	originalChannels = manager()->ConfiguredChannels();
	originalService = obs_frontend_get_streaming_service();
	original = obs_frontend_get_current_scene();
	studioMode = obs_frontend_preview_program_mode_active();
	char *p = obs_frontend_get_current_profile();
	originalProfile = QString::fromUtf8(p);
	bfree(p);
	qaProfile = "Simultaneous QA " + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
	auto *u = obs_frontend_get_user_config();
	for (const char *key : {"StudioMultiCanvas", "WarnBeforeStartingStream", "WarnBeforeStoppingStream",
				"RecordWhenStreaming", "ReplayBufferWhileStreaming"}) {
		userValues.push_back({key, config_has_user_value(u, "BasicWindow", key),
				      QString::fromUtf8(config_get_string(u, "BasicWindow", key))});
	}
	for (const char *key : {"WarnBeforeStartingStream", "WarnBeforeStoppingStream", "RecordWhenStreaming",
				"ReplayBufferWhileStreaming"}) {
		config_set_bool(u, "BasicWindow", key, false);
	}
	config_set_bool(u, "BasicWindow", "StudioMultiCanvas", true);
	obs_frontend_duplicate_profile(qaProfile.toUtf8().constData());
	duplicated = true;
	later(800, [] {
		char *p = obs_frontend_get_current_profile();
		check(QString::fromUtf8(p) == qaProfile, "output test uses a temporary duplicate profile");
		bfree(p);
		if (studioMode) {
			obs_frontend_set_preview_program_mode(false);
		}
		fixture = obs_scene_create("Simultaneous QA — local media");
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_bool(d, "is_local_file", true);
		obs_data_set_bool(d, "looping", true);
		obs_data_set_string(d, "local_file", (root + "/fixture.mp4").toUtf8().constData());
		media = obs_source_create("ffmpeg_source", "Simultaneous QA — pattern and tone", d, nullptr);
		obs_source_set_audio_mixers(media, 1);
		obs_scene_add(fixture, media);
		obs_frontend_set_current_scene(obs_scene_get_source(fixture));
		later(800, [] {
			portraitCanvas = vertical();
			OBSSourceAutoRelease p = obs_canvas_get_channel(portraitCanvas, 0);
			portraitScene = obs_scene_from_source(p);
			check(bool(portraitScene), "persistent independent portrait scene ready");
			OBSDataAutoRelease d = obs_data_create();
			obs_data_set_int(d, "width", 160);
			obs_data_set_int(d, "height", 160);
			obs_data_set_int(d, "color", 0xff33bb55);
			OBSSourceAutoRelease color =
				obs_source_create_private("color_source_v3", "simultaneous-portrait-marker", d);
			marker = obs_scene_add(portraitScene, color);
			vec2 pos{40.f, 40.f};
			obs_sceneitem_set_pos(marker, &pos);
			for (int i = 0; i < 2; ++i) {
				QTcpServer freePort;
				check(freePort.listen(QHostAddress::LocalHost, 0), "local receiver port available");
				ports[i] = freePort.serverPort();
				receivers[i] = new QProcess(qApp);
				receivers[i]->setProcessChannelMode(QProcess::MergedChannels);
			}
			phaseRun();
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
	root = qEnvironmentVariable("OBS_STUDIO_SIMULTANEOUS_TEST_ROOT");
	server = qEnvironmentVariable("OBS_STUDIO_SIMULTANEOUS_SERVER");
	if (root.isEmpty() || server.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	later(180000, [] { finish("Simultaneous output test timed out"); });
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
