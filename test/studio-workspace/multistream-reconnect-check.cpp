/* Opt-in native integration test. Real production manager, encoders and two
 * MediaMTX RTMP receivers bound exclusively to loopback. Never reads account storage.
 * OBS_STUDIO_RECONNECT_SERVER selects the MediaMTX executable.
 * OBS_STUDIO_RECONNECT_TEST_ROOT selects the private evidence directory.
 * QProcess/QTimer/libobs keep the probe portable between Windows and Linux. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

OBS_DECLARE_MODULE()
static QJsonArray persistence;
bool MultistreamChannelStore::SetEnabled(const std::string &id, bool enabled, std::string &error)
{
	if (id != "reconnect-local-a" && id != "reconnect-local-b") {
		error = "The probe cannot persist real channels";
		return false;
	}
	persistence.append(QJsonObject{{"id", QString::fromStdString(id)}, {"enabled", enabled}});
	error.clear();
	return true;
}
namespace {
QString root, serverBinary;
QJsonArray checks, transitions;
QElapsedTimer clock;
std::unique_ptr<MultiStreamManager> manager;
OBSEncoderAutoRelease video, audio;
OBSCanvasAutoRelease canvas;
OBSSceneAutoRelease scene;
OBSSourceAutoRelease color;
QTimer *timer = nullptr;
QProcess *receivers[2] = {};
bool autoListen[2] = {true, true};
int ports[2] = {}, segments[2] = {};
bool finished = false, trackingA = false, interruptedA = false;
int stage = 0, expectedReconnects = 0, expectedBRecordings = 0;
qint64 entered = 0, deadline = 0;
uint64_t beforeA = 0;
QString stateName(MultiStreamChannelState state)
{
	switch (state) {
	case MultiStreamChannelState::Idle:
		return "idle";
	case MultiStreamChannelState::Starting:
		return "starting";
	case MultiStreamChannelState::Live:
		return "live";
	case MultiStreamChannelState::Stopping:
		return "stopping";
	case MultiStreamChannelState::Failed:
		return "failed";
	}
	return "unknown";
}
void check(bool passed, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", passed}});
	if (!passed) {
		throw std::runtime_error(name);
	}
}
void finish(const QString &error = {})
{
	if (finished) {
		return;
	}
	finished = true;
	if (timer) {
		timer->stop();
	}
	trackingA = false;
	if (manager) {
		manager->SetStateCallback({});
		manager->Stop(true);
	}
	autoListen[0] = autoListen[1] = false;
	QTimer::singleShot(1800, qApp, [error]() {
		manager.reset();
		for (auto *receiver : receivers) {
			if (receiver && receiver->state() != QProcess::NotRunning) {
				receiver->kill();
			}
		}
		video = nullptr;
		audio = nullptr;
		if (canvas) {
			obs_canvas_set_channel(canvas, 0, nullptr);
		}
		color = nullptr;
		scene = nullptr;
		canvas = nullptr;
		QFile report(root + "/runtime.json");
		if (report.open(QIODevice::WriteOnly)) {
			report.write(
				QJsonDocument(
					QJsonObject{
						{"passed", error.isEmpty()},
						{"error", error},
						{"checks", checks},
						{"transitions", transitions},
						{"persistence", persistence},
						{"elapsed_ms", clock.elapsed()},
						{"platform", QSysInfo::productType()},
						{"receivers", "two independent MediaMTX RTMP servers on 127.0.0.1 only"},
						{"scope",
						 "real production manager, shared H.264/AAC encoders, manual/automatic reconnect, cancellation; no public platform or OAuth"}})
					.toJson());
		}
	});
}
void listen(int index)
{
	if (finished || !autoListen[index]) {
		return;
	}
	const QString label = QString("%1-%2").arg(index ? "b" : "a").arg(++segments[index], 2, 10, QLatin1Char('0'));
	QString recordingPath = QDir::fromNativeSeparators(root + "/" + (index ? "b" : "a") + "/%path/%Y-%m-%d_%H-%M-%S-%f");
	recordingPath.replace("'", "''");
	QFile configuration(root + "/" + label + ".yml");
	check(configuration.open(QIODevice::WriteOnly), "local RTMP server config writable");
	configuration.write(
		QString("logLevel: info\nrtsp: false\nrtmp: true\nrtmpAddress: 127.0.0.1:%1\n"
			"hls: false\nwebrtc: false\nsrt: false\nmoq: false\napi: false\nmetrics: false\npprof: false\nplayback: false\n"
			"pathDefaults:\n  record: true\n  recordPath: '%2'\n  recordFormat: fmp4\n  recordPartDuration: 100ms\n"
			"  recordSegmentDuration: 1h\n  recordDeleteAfter: 0s\npaths:\n  all_others: {}\n")
			.arg(ports[index])
			.arg(recordingPath)
			.toUtf8());
	configuration.close();
	receivers[index]->setProcessChannelMode(QProcess::MergedChannels);
	receivers[index]->setStandardOutputFile(root + "/" + label + ".log");
	receivers[index]->start(serverBinary, {configuration.fileName()});
}
MultiStreamChannelSnapshot snapshot(int index)
{
	for (const auto &entry : manager->Snapshot()) {
		if (entry.id == (index ? "reconnect-local-b" : "reconnect-local-a")) {
			return entry;
		}
	}
	throw std::runtime_error("missing destination snapshot");
}
OBSOutputAutoRelease output(int index)
{
	for (const auto &entry : manager->ChannelOutputs()) {
		if (entry.id == (index ? "reconnect-local-b" : "reconnect-local-a")) {
			return obs_output_get_ref(entry.output);
		}
	}
	return {};
}
int recordings(int index)
{
	QDirIterator files(root + (index ? "/b" : "/a"), {"*.mp4"}, QDir::Files, QDirIterator::Subdirectories);
	int count = 0;
	while (files.hasNext()) { files.next(); if (files.fileInfo().size() > 10000) ++count; }
	return count;
}
bool live(int index)
{
	return snapshot(index).state == MultiStreamChannelState::Live && snapshot(index).health.totalBytes > 2000;
}
uint64_t bytesA()
{
	return snapshot(0).health.totalBytes;
}
void next(int value, int timeout = 10000)
{
	stage = value;
	entered = clock.elapsed();
	deadline = entered + timeout;
}
void unchangedA(const char *name)
{
	check(!interruptedA && live(0) && bytesA() > beforeA && receivers[0]->state() == QProcess::Running &&
		      segments[0] == 1,
	      name);
	beforeA = bytesA();
}
void tick()
{
	try {
		if (clock.elapsed() > deadline) {
			throw std::runtime_error(("timeout at stage " + std::to_string(stage)).c_str());
		}
		const qint64 elapsed = clock.elapsed() - entered;
		QFile progress(root + "/progress.json");
		if (progress.open(QIODevice::WriteOnly)) {
			progress.write(
				QJsonDocument(
					QJsonObject{{"stage", stage}, {"elapsed", elapsed}, {"at_ms", clock.elapsed()}})
					.toJson());
		}
		std::string error;
		switch (stage) {
		case 0: {
			if (elapsed < 1000) {
				return;
			}
			check(receivers[0]->state() == QProcess::Running && receivers[1]->state() == QProcess::Running,
			      "two independent loopback RTMP listeners start");
			obs_video_info info{};
			check(obs_get_video_info(&info), "native video pipeline ready");
			info.base_width = info.output_width = 320;
			info.base_height = info.output_height = 180;
			/* Private canvases are rendered on the global cadence; keep the
			 * encoder timebase equal to it so reconnect sync is meaningful. */
			canvas = obs_canvas_create_private("reconnect-test-canvas", &info,
							   ACTIVATE | SCENE_REF | EPHEMERAL);
			check(bool(canvas), "private canvas avoids modifying the user's scenes");
			scene = obs_canvas_scene_create(canvas, "reconnect-test-scene");
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_int(settings, "color", 0xff508030);
			obs_data_set_int(settings, "width", 320);
			obs_data_set_int(settings, "height", 180);
			color = obs_source_create_private("color_source_v3", "reconnect-test-pattern", settings);
			check(bool(scene) && bool(color), "private video pattern created");
			obs_scene_add(scene, color);
			obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
			obs_data_set_string(settings, "rate_control", "CBR");
			obs_data_set_int(settings, "bitrate", 500);
			obs_data_set_string(settings, "preset", "ultrafast");
			obs_data_set_int(settings, "keyint_sec", 1);
			video = obs_video_encoder_create("obs_x264", "reconnect-test-video", settings, nullptr);
			audio = obs_audio_encoder_create("ffmpeg_aac", "reconnect-test-audio", nullptr, 5, nullptr);
			check(bool(video) && bool(audio), "portable H.264 and AAC encoders created");
			obs_encoder_set_video(video, obs_canvas_get_video(canvas));
			obs_encoder_set_audio(audio, obs_get_audio());
			manager = std::make_unique<MultiStreamManager>();
			MultiStreamChannel a;
			a.id = "reconnect-local-a";
			a.displayName = "Local A";
			a.audioMixIndex = 5;
			a.streamKey = "test";
			a.server = QString("rtmp://127.0.0.1:%1/live").arg(ports[0]).toStdString();
			MultiStreamChannel b = a;
			b.id = "reconnect-local-b";
			b.displayName = "Local B";
			b.server = QString("rtmp://127.0.0.1:%1/live").arg(ports[1]).toStdString();
			check(manager->Configure({a, b}, error), "production manager configures local destinations");
			manager->SetStateCallback([](const auto &s) {
				QMetaObject::invokeMethod(
					qApp,
					[s]() {
						transitions.append(QJsonObject{{"id", QString::fromStdString(s.id)},
									       {"state", stateName(s.state)},
									       {"at_ms", clock.elapsed()},
									       {"reconnects", s.health.reconnects}});
						if (trackingA && s.id == "reconnect-local-a" &&
						    s.state != MultiStreamChannelState::Live) {
							interruptedA = true;
						}
					},
					Qt::QueuedConnection);
			});
			MultiStreamOutputSettings options;
			options.reconnect = {20, 1};
			options.bindIp = "127.0.0.1";
			check(manager->Start(video, audio, options, error),
			      "production manager starts both destinations");
			next(1);
			break;
		}
		case 1:
			if (!live(0) || !live(1) || elapsed < 2000 || recordings(0) < 1 || recordings(1) < 1) {
				return;
			}
			check(obs_output_get_video_encoder(output(0)) == video.Get() &&
				      obs_output_get_video_encoder(output(1)) == video.Get(),
			      "destinations share one video encoder");
			trackingA = true;
			beforeA = bytesA();
			expectedReconnects = snapshot(1).health.reconnects + 1;
			expectedBRecordings = recordings(1) + 1;
			check(manager->RestartChannel("reconnect-local-b", error), "manual reconnect accepted");
			check(manager->RestartChannel("reconnect-local-b", error) &&
				      manager->RestartChannel("reconnect-local-b", error),
			      "repeated reconnect clicks coalesce");
			check(persistence.isEmpty() && manager->ConfiguredChannels()[1].enabled,
			      "manual reconnect does not toggle or persist channel settings");
			next(2);
			break;
		case 2:
			if (!live(1) || elapsed < 2000 || recordings(1) < expectedBRecordings) {
				return;
			}
			check(snapshot(1).health.reconnects == expectedReconnects,
			      "manual reconnect restarts exactly once");
			unchangedA("destination A keeps receiving during manual reconnect of B; B records new audio/video");
			expectedBRecordings = recordings(1) + 1;
			autoListen[1] = false;
			receivers[1]->kill();
			next(3, 15000);
			break;
		case 3:
			if (elapsed < 3000 || snapshot(1).state != MultiStreamChannelState::Starting) {
				return;
			}
			check(snapshot(1).health.reconnects > expectedReconnects,
			      "network loss enters automatic reconnect");
			unchangedA("destination A keeps receiving while B is offline");
			autoListen[1] = true;
			listen(1);
			next(4, 18000);
			break;
		case 4:
			if (!live(1) || elapsed < 2000 || recordings(1) < expectedBRecordings) {
				return;
			}
			check(snapshot(1).state == MultiStreamChannelState::Live,
			      "automatic reconnect returns B to Live");
			unchangedA("destination A remains continuous when B recovers");
			expectedReconnects = snapshot(1).health.reconnects + 1;
			expectedBRecordings = recordings(1) + 1;
			check(manager->SetChannelEnabled("reconnect-local-b", false, error) &&
				      manager->SetChannelEnabled("reconnect-local-b", true, error),
			      "rapid off/on accepted");
			next(5);
			break;
		case 5:
			if (!live(1) || elapsed < 2000 || recordings(1) < expectedBRecordings || snapshot(1).health.reconnects < expectedReconnects) {
				return;
			}
			check(persistence.size() == 2, "only explicit off/on updates enabled settings");
			unchangedA("rapid off/on of B leaves A sending");
			check(manager->RestartChannel("reconnect-local-b", error) &&
				      manager->SetChannelEnabled("reconnect-local-b", false, error),
			      "disable cancels queued reconnect");
			next(6);
			break;
		case 6:
			if (elapsed < 3000) {
				return;
			}
			{
				OBSOutputAutoRelease b;
				obs_enum_outputs(
					[](void *p, obs_output_t *o) {
						if (std::string(obs_output_get_name(o)) ==
						    "multistream_output_reconnect-local-b") {
							*static_cast<OBSOutputAutoRelease *>(p) = obs_output_get_ref(o);
						}
						return true;
					},
					&b);
				check(b && !obs_output_active(b) &&
					      snapshot(1).state != MultiStreamChannelState::Starting,
				      "disabled destination stays stopped after queued work drains");
			}
			unchangedA("disabling B does not stop A");
			check(!manager->RestartChannel("reconnect-local-b", error),
			      "manual reconnect refuses disabled channel");
			expectedBRecordings = recordings(1) + 1;
			check(manager->SetChannelEnabled("reconnect-local-b", true, error),
			      "disabled destination can be explicitly enabled again");
			next(7);
			break;
		case 7:
			if (!live(1) || elapsed < 2000 || recordings(1) < expectedBRecordings) {
				return;
			}
			unchangedA("A keeps sending across B disable and enable");
			trackingA = false;
			check(manager->RestartChannel("reconnect-local-b", error),
			      "reconnect queued immediately before global stop");
			// The primary output can deliver stop from a libobs worker.
			std::thread([]() { manager->Stop(); }).join();
			next(8);
			break;
		case 8:
			if (elapsed < 3000) {
				return;
			}
			check(!manager->IsActive() && !obs_output_active(output(0)) && !obs_output_active(output(1)),
			      "worker-thread global stop cancels pending reconnect and stops both outputs");
			check(!manager->RestartChannel("reconnect-local-b", error),
			      "stopped session cannot be resurrected by reconnect");
			check(manager->ConfiguredChannels()[0].enabled && manager->ConfiguredChannels()[1].enabled,
			      "global stop preserves configured enabled flags");
			{
				auto channels = manager->ConfiguredChannels();
				check(manager->Configure(channels, error),
				      "manager can be configured for a new session after stopping");
				MultiStreamOutputSettings options;
				options.reconnect = {20, 1};
				options.bindIp = "127.0.0.1";
				expectedBRecordings = recordings(1) + 1;
				check(manager->Start(video, audio, options, error),
				      "a fresh session can start after cancellation");
			}
			next(9);
			break;
		case 9:
			if (!live(0) || !live(1) || elapsed < 2000 || recordings(0) < 2 || recordings(1) < expectedBRecordings) {
				return;
			}
			check(manager->RestartChannel("reconnect-local-b", error),
			      "reconnect queued immediately before manager destruction");
			manager->SetStateCallback({});
			manager.reset();
			next(10);
			break;
		case 10:
			if (elapsed < 2000) {
				return;
			}
			check(!interruptedA, "A had no non-Live transition throughout all B failures and retries");
			check(true, "destroying manager cancels queued callbacks without crash");
			finish();
			break;
		}
	} catch (const std::exception &error) {
		finish(QString::fromUtf8(error.what()));
	}
}
void run()
{
	try {
		check(!obs_frontend_streaming_active() && !obs_frontend_recording_active() &&
			      !obs_frontend_replay_buffer_active(),
		      "user outputs idle before probe");
		check(QFile::exists(serverBinary), "MediaMTX test server available");
		for (int i = 0; i < 2; ++i) {
			QTcpServer reservation;
			check(reservation.listen(QHostAddress::LocalHost), "free loopback port reserved");
			ports[i] = reservation.serverPort();
			reservation.close();
			receivers[i] = new QProcess(qApp);
			QObject::connect(receivers[i], &QProcess::finished, qApp, [i](int, QProcess::ExitStatus) {
				if (!finished && autoListen[i]) {
					finish("Local RTMP test server unexpectedly exited");
				}
			});
			listen(i);
		}
		clock.start();
		next(0);
		timer = new QTimer(qApp);
		QObject::connect(timer, &QTimer::timeout, qApp, tick);
		timer->start(100);
	} catch (const std::exception &error) {
		finish(QString::fromUtf8(error.what()));
	}
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(1500, qApp, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_RECONNECT_TEST_ROOT");
	serverBinary = qEnvironmentVariable("OBS_STUDIO_RECONNECT_SERVER");
	if (root.isEmpty() || !QDir().mkpath(root)) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
	if (timer) {
		timer->stop();
	}
	autoListen[0] = autoListen[1] = false;
	manager.reset();
	for (auto *receiver : receivers) {
		if (receiver) {
			receiver->disconnect();
			receiver->kill();
			receiver->waitForFinished(3000);
			delete receiver;
		}
	}
	video = nullptr;
	audio = nullptr;
	if (canvas) {
		obs_canvas_set_channel(canvas, 0, nullptr);
	}
	color = nullptr;
	scene = nullptr;
	canvas = nullptr;
}
