/* Native opt-in CUE/VOD measurement. All captures are private local files.
 * The parent owns OBS lifecycle, isolated Pulse sink and physical endpoint.
 * No stream is started, and physical microphone input is never monitored.
 * Live/VOD/record below denote separately encoded local mixer paths, not a
 * certification of Twitch ingest or acoustic headphone latency. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTimer>
#include <QUuid>
#include <array>
#include <atomic>
#include <thread>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	error = "Audio fixture never persists destinations";
	return false;
}
namespace {
QString root, configRoot, virtualSink, physicalSink, micDevice, ffmpeg;
QString originalProfile, qaProfile, monitorName, monitorId, originalSceneUuid;
QMainWindow *window = nullptr;
QWidget *strip = nullptr;
QJsonArray checks, stages;
QJsonObject facts;
QByteArray originalLayout;
bool done = false, duplicated = false, ready = false, recordingStarted = false, studioMode = false;
int stageIndex = 0;
std::atomic<bool> toneStop{false}, toneFailed{false};
std::thread toneWorker;
std::atomic<uint64_t> toneBlocks{0}, toneLateBlocks{0}, toneCatchups{0}, toneMaxLate{0}, toneContinuityErrors{0};
struct ToneClockTrace { uint64_t sampleIndex, timestamp, dispatchStart, dispatchEnd; };
std::array<ToneClockTrace, 8192> toneTrace{};
size_t toneTraceCount = 0; // Worker-only until joined; report reads afterward.
QTimer *toneTimer = nullptr;
QProcess *captureProcess = nullptr;
OBSSceneAutoRelease scene;
OBSSourceAutoRelease music, voice, reference, microphone, originalScene;
std::array<OBSSource, 5> globalAudio;
OBSService originalService;
std::vector<MultiStreamChannel> originalChannels;
OBSOutputAutoRelease recording;
OBSEncoderAutoRelease video;
std::array<OBSEncoder, 4> encoders;
std::array<std::unique_ptr<QFile>, 4> pcm;
std::array<uint64_t, 4> frameCounts{};
std::mutex pcmMutex;
bool capturePcm = false;
double videoFps = 60.;
std::vector<size_t> connected;
struct Stage {
	QString name;
	std::function<void()> setup;
	bool music[3];
	bool monitor;
	bool voice = true;
	bool physical = false;
	bool mic = false;
};
std::vector<Stage> sequence;
QJsonObject currentStage;
void finish(QString error = {});
void check(bool pass, const char *label)
{
	checks.append(QJsonObject{{"check", label},
				  {"passed", pass},
				  {"stage", stageIndex},
				  {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
	QSaveFile f(root + "/progress.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"checks", checks}, {"stage", stageIndex}, {"last_check", label}})
				.toJson());
		f.commit();
	}
	if (!pass) {
		throw std::runtime_error(label);
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
void capture(void *, size_t mix, audio_data *data)
{
	if (mix >= 4 || !data->data[0]) {
		return;
	}
	std::lock_guard lock(pcmMutex);
	if (capturePcm && pcm[mix]) {
		pcm[mix]->write(reinterpret_cast<char *>(data->data[0]), data->frames * sizeof(float));
		frameCounts[mix] += data->frames;
	}
}
void stopToneWorker()
{
 if (toneTimer) toneTimer->stop();
 toneStop.store(true);
 if (toneWorker.joinable()) toneWorker.join();
}
void startToneWorker(std::array<obs_source_t *, 3> inputs)
{
 toneStop.store(false);
 toneWorker = std::thread([inputs] {
  std::array<OBSSourceAutoRelease, 3> sources;
  for (size_t i = 0; i < sources.size(); ++i) sources[i] = obs_source_get_ref(inputs[i]);
  const uint64_t anchor = os_gettime_ns();
  uint64_t sampleIndex = 0, previousTimestamp = 0;
  unsigned consecutiveCatchups = 0;
  while (!toneStop.load()) {
   const uint64_t timestamp = anchor + sampleIndex * 1000000000ULL / 48000;
   os_sleepto_ns_fast(timestamp);
   if (toneStop.load()) break;
   const uint64_t dispatch = os_gettime_ns();
   const uint64_t late = dispatch > timestamp ? dispatch - timestamp : 0;
   if (late > toneMaxLate.load()) toneMaxLate.store(late);
   if (late > 1000000) toneLateBlocks.fetch_add(1);
   if (late >= 20000000) { toneCatchups.fetch_add(1); ++consecutiveCatchups; }
   else consecutiveCatchups = 0;
   if (late > 100000000 || consecutiveCatchups > 3 || toneTraceCount >= toneTrace.size()) {
    toneFailed.store(true); break; // Never drop, reanchor or fabricate silence.
   }
   if (previousTimestamp && timestamp - previousTimestamp != 20000000) toneContinuityErrors.fetch_add(1);
   previousTimestamp = timestamp;
   float a[960], b[960], r[960];
   for (size_t i = 0; i < 960; ++i) {
    double t = double(sampleIndex + i) / 48000.;
    bool on = ((sampleIndex + i) % 48000) < 9600;
    a[i] = on * .02 * std::sin(2 * 3.141592653589793 * 997 * t);
    b[i] = .015 * std::sin(2 * 3.141592653589793 * 431 * t);
    r[i] = on * .012 * std::sin(2 * 3.141592653589793 * 2003 * t);
   }
   obs_source_audio data{};
   data.frames = 960; data.samples_per_sec = 48000; data.speakers = SPEAKERS_MONO;
   data.format = AUDIO_FORMAT_FLOAT; data.timestamp = timestamp;
   std::array<float *, 3> samples{a, b, r};
   for (size_t i = 0; i < sources.size(); ++i) {
    data.data[0] = reinterpret_cast<uint8_t *>(samples[i]);
    obs_source_output_audio(sources[i], &data);
   }
   toneTrace[toneTraceCount++] = {sampleIndex, timestamp, dispatch, os_gettime_ns()};
   sampleIndex += 960; toneBlocks.fetch_add(1);
  }
 });
 toneTimer = new QTimer(qApp); toneTimer->setInterval(100);
 QObject::connect(toneTimer, &QTimer::timeout, qApp, [] {
  if (!done && toneFailed.load()) finish("synthetic source worker exceeded bounded sample-clock backlog");
 });
 toneTimer->start();
}
void writeToneClockTrace()
{
 // Called only after stop/join; no worker writes UI objects or files.
 QJsonArray trace;
 for (size_t i = 0; i < toneTraceCount; ++i) {
  const auto &t = toneTrace[i];
  trace.append(QJsonObject{{"sample_index", qint64(t.sampleIndex)}, {"timestamp_ns", qint64(t.timestamp)},
                          {"dispatch_start_ns", qint64(t.dispatchStart)}, {"dispatch_end_ns", qint64(t.dispatchEnd)}});
 }
 QJsonObject clock{{"blocks", qint64(toneBlocks.load())}, {"maximum_lateness_ns", qint64(toneMaxLate.load())},
                   {"late_blocks_gt_1ms", qint64(toneLateBlocks.load())}, {"catchup_blocks_ge_20ms", qint64(toneCatchups.load())},
                   {"continuity_errors", qint64(toneContinuityErrors.load())}, {"backlog_failed", toneFailed.load()},
                   {"worker_joined", !toneWorker.joinable()}, {"trace", trace}};
 facts["source_clock"] = QJsonObject{{"blocks", qint64(toneBlocks.load())}, {"maximum_lateness_ns", qint64(toneMaxLate.load())},
                                     {"continuity_errors", qint64(toneContinuityErrors.load())}, {"worker_joined", !toneWorker.joinable()},
                                     {"backlog_failed", toneFailed.load()}};
 QSaveFile f(root + "/source-clock.json");
 if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(clock).toJson()); f.commit(); }
}
void writeReport(QString error)
{
	writeToneClockTrace();
	QSaveFile f(root + "/runtime.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"stages", stages},
					{"facts", facts},
					{"linux_native", true},
					{"windows_native", false},
					{"scope",
					 "Actual Qt controls, libobs mixed PCM and four locally encoded AAC mixes, explicit Pulse virtual/physical sink monitor captures, private physical input. No public streaming, no acoustic latency/audition claim, no Twitch archive ingest certification."}})
				.toJson());
		f.commit();
	}
}
void restore(QString error, std::shared_ptr<QElapsedTimer> clock)
{
	if (recording && obs_output_active(recording)) {
		if (clock->elapsed() > 15000) {
			writeReport(error + "; output stop deadline, private fixture retained");
			return;
		}
		QTimer::singleShot(100, qApp, [error, clock] { restore(error, clock); });
		return;
	}
	for (auto mix : connected) {
		audio_output_disconnect(obs_get_audio(), mix, capture, nullptr);
	}
	connected.clear();
	recording = nullptr;
	video = nullptr;
	for (auto &e : encoders) {
		e = nullptr;
	}
	if (!ready) {
		writeReport(error);
		return;
	}
	obs_set_audio_monitoring_device(monitorName.toUtf8().constData(), monitorId.toUtf8().constData());
	if (originalScene) {
		obs_frontend_set_current_scene(originalScene);
	}
	for (size_t i = 0; i < globalAudio.size(); ++i) {
		obs_set_output_source(unsigned(i + 1), globalAudio[i]);
	}
	for (auto *s : {microphone.Get(), music.Get(), voice.Get(), reference.Get(),
			scene ? obs_scene_get_source(scene) : nullptr}) {
		if (s) {
			obs_source_remove(s);
		}
	}
	microphone = nullptr;
	music = nullptr;
	voice = nullptr;
	reference = nullptr;
	scene = nullptr;
	if (studioMode) {
		obs_frontend_set_preview_program_mode(true);
	}
	std::string failure;
	manager()->Configure(originalChannels, failure);
	obs_frontend_set_streaming_service(originalService);
	window->restoreState(originalLayout);
	if (duplicated) {
		obs_frontend_set_current_profile(originalProfile.toUtf8().constData());
		obs_frontend_delete_profile(qaProfile.toUtf8().constData());
		duplicated = false;
	}
	obs_frontend_save();
	try {
		check(profile() == originalProfile, "original private profile restored");
		check(!obs_video_active(), "all test encoders stopped before cleanup");
		const char *n = nullptr, *id = nullptr;
		obs_get_audio_monitoring_device(&n, &id);
		check(QString::fromUtf8(id) == monitorId, "original OBS monitor endpoint restored");
	} catch (const std::exception &e) {
		if (error.isEmpty()) {
			error = QString::fromUtf8(e.what());
		}
	}
	/* The frontend owns the restored objects now. Module globals must not
	 * release stale libobs refs from process-exit destructors after shutdown. */
	for (auto &saved : globalAudio)
		saved = nullptr;
	originalScene = nullptr;
	originalService = nullptr;
	writeReport(error);
}
void finish(QString error)
{
	if (done) {
		return;
	}
	done = true;
	stopToneWorker();
	if (error.isEmpty() && (toneFailed.load() || toneContinuityErrors.load() != 0))
		error = "synthetic source failed sample-clock continuity or bounded backlog";
	if (captureProcess && captureProcess->state() != QProcess::NotRunning) {
		captureProcess->kill();
	}
	{
		std::lock_guard lock(pcmMutex);
		capturePcm = false;
		for (auto &f : pcm) {
			if (f) {
				f->close();
				f.reset();
			}
		}
	}
	if (recordingStarted && recording && obs_output_active(recording)) {
		obs_output_stop(recording);
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
void click(const char *n)
{
	auto *b = child<QPushButton>(strip, n);
	check(b->isEnabled(), "native strip control enabled");
	b->click();
}
void mode(obs_monitoring_type type)
{
	auto *c = child<QComboBox>(window, "studioAudioMonitor");
	int i = c->findData(int(type));
	check(i >= 0, "monitor enum exists in actual audio dock");
	c->setCurrentIndex(i);
	check(QMetaObject::invokeMethod(c, "activated", Qt::DirectConnection, Q_ARG(int, i)),
	      "native monitor mode activation delivered");
	check(obs_source_get_monitoring_type(music) == type, "dock monitor selection reaches actual backend");
}
void endpoint(const QString &id)
{
	auto *c = child<QComboBox>(window, "studioAudioMonitorDevice");
	const int i = c->findData(id);
	check(i >= 0, "explicit monitoring endpoint is enumerated");
	c->setCurrentIndex(i);
	check(QMetaObject::invokeMethod(c, "activated", Qt::DirectConnection, Q_ARG(int, i)),
	      "native monitoring endpoint activation delivered");
	const char *n = nullptr, *actual = nullptr;
	obs_get_audio_monitoring_device(&n, &actual);
	check(QString::fromUtf8(actual) == id, "native endpoint selection changes OBS monitoring only");
}
void delay(int value)
{
	auto *s = child<QSpinBox>(window, "studioAudioSync");
	s->setValue(value);
	check(QMetaObject::invokeMethod(s, "editingFinished", Qt::DirectConnection), "native delay edit committed");
}
void persist()
{
	OBSDataAutoRelease saved = obs_save_source(music);
	QFile f(root + "/saved-source.json");
	check(f.open(QIODevice::WriteOnly), "private source persistence file opens");
	f.write(obs_data_get_json(saved));
	f.close();
	QFile read(root + "/saved-source.json");
	check(read.open(QIODevice::ReadOnly), "saved source can be read back");
	OBSDataAutoRelease data = obs_data_create_from_json(read.readAll().constData());
	obs_data_set_string(data, "uuid", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8().constData());
	obs_data_set_string(data, "name", "Audio QA persistence reload");
	OBSSourceAutoRelease loaded = obs_load_source(data);
	check(bool(loaded), "source settings are recreated through native load path");
	const bool matches = obs_source_muted(loaded) == obs_source_muted(music) &&
			     obs_source_get_monitoring_type(loaded) == obs_source_get_monitoring_type(music) &&
			     obs_source_get_audio_mixers(loaded) == obs_source_get_audio_mixers(music) &&
			     obs_source_get_sync_offset(loaded) == obs_source_get_sync_offset(music);
	obs_source_remove(loaded);
	check(matches, "mute monitoring mixes and delay survive source serialization and native reload");
}
void next();
void finishStage(std::shared_ptr<QElapsedTimer> clock)
{
	if (captureProcess->state() != QProcess::NotRunning) {
		check(clock->elapsed() < 9000, "explicit Pulse monitor capture completes before deadline");
		later(100, [clock] { finishStage(clock); });
		return;
	}
	QFile log(root + "/" + sequence[stageIndex].name + "-capture.log");
	if (log.open(QIODevice::WriteOnly)) {
		log.write(captureProcess->readAllStandardError());
	}
	check(captureProcess->exitStatus() == QProcess::NormalExit && captureProcess->exitCode() == 0,
	      "finite capture from the explicitly selected sink monitor succeeds");
	{
		std::lock_guard lock(pcmMutex);
		capturePcm = false;
		QJsonArray counts;
		for (size_t i = 0; i < pcm.size(); ++i) {
			pcm[i]->close();
			pcm[i].reset();
			counts.append(qint64(frameCounts[i]));
		}
		currentStage["pcm_frames"] = counts;
	}
	currentStage["file_end_seconds"] = double(obs_output_get_total_frames(recording)) / videoFps;
	currentStage["actual_mask"] = int(obs_source_get_audio_mixers(music));
	currentStage["actual_muted"] = obs_source_muted(music);
	currentStage["actual_monitoring"] = int(obs_source_get_monitoring_type(music));
	currentStage["sync_offset_ns"] = qint64(obs_source_get_sync_offset(music));
	stages.append(currentStage);
	captureProcess->deleteLater();
	captureProcess = nullptr;
	if (++stageIndex == int(sequence.size())) {
		finish();
	} else {
		later(200, next);
	}
}
void beginStage()
{
	const auto &s = sequence[stageIndex];
	currentStage = QJsonObject{{"name", s.name},
				   {"expected_music", QJsonArray{s.music[0], s.music[1], s.music[2]}},
				   {"expected_monitor_music", s.monitor},
				   {"expected_voice", s.voice},
				   {"physical_endpoint", s.physical},
				   {"physical_input", s.mic},
				   {"file_start_seconds", double(obs_output_get_total_frames(recording)) / videoFps},
				   {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
	{
		std::lock_guard lock(pcmMutex);
		for (size_t i = 0; i < pcm.size(); ++i) {
			frameCounts[i] = 0;
			pcm[i] = std::make_unique<QFile>(root + "/" + s.name + QString("-mix%1.f32").arg(i));
			check(pcm[i]->open(QIODevice::WriteOnly), "private per-mix PCM file opens");
		}
		capturePcm = true;
	}
	QSaveFile status(root + "/current-stage.json");
	if (status.open(QIODevice::WriteOnly)) {
		status.write(QJsonDocument(currentStage).toJson());
		status.commit();
	}
	captureProcess = new QProcess(qApp);
	captureProcess->setProgram(ffmpeg);
	captureProcess->setArguments({"-nostdin", "-v", "error", "-f", "pulse", "-i",
				      (s.physical ? physicalSink : virtualSink) + ".monitor", "-t", "3", "-ar", "48000",
				      "-ac", "1", "-c:a", "pcm_f32le", root + "/" + s.name + "-monitor.wav"});
	captureProcess->start();
	auto clock = std::make_shared<QElapsedTimer>();
	clock->start();
	later(100, [clock] { finishStage(clock); });
}
void next()
{
	sequence[stageIndex].setup();
	later(1000, beginStage);
}
void prepareSteps()
{
	sequence = {{"baseline", [] {}, {true, true, true}, false},
		    {"cue_both",
		     [] {
			     click("studioMixerCue");
			     check(obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
				   "CUE enables monitor plus output");
			     action("actionMainUndo");
			     check(obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_NONE,
				   "one Undo restores CUE off");
			     action("actionMainRedo");
		     },
		     {true, true, true},
		     true},
		    {"muted_both",
		     [] {
			     click("studioMixerMute");
			     check(obs_source_muted(music) && obs_source_get_monitoring_type(music) ==
								      OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
				   "MUTE preserves monitor plus output enum");
		     },
		     {false, false, false},
		     true},
		    {"unmuted_both",
		     [] {
			     click("studioMixerMute");
			     check(!obs_source_muted(music), "MUTE button restores program mute only");
			     action("actionMainUndo");
			     check(obs_source_muted(music) && obs_source_get_monitoring_type(music) ==
								      OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
				   "one Undo of MUTE preserves CUE mode");
			     action("actionMainRedo");
		     },
		     {true, true, true},
		     true},
		    {"monitor_only", [] { mode(OBS_MONITORING_TYPE_MONITOR_ONLY); }, {false, false, false}, true},
		    {"muted_only",
		     [] {
			     check(!child<QPushButton>(strip, "studioMixerMute")->isChecked(),
				   "Monitor Only is not falsely shown as muted");
			     click("studioMixerMute");
			     check(obs_source_muted(music) &&
					   obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_MONITOR_ONLY,
				   "Mute in Monitor Only leaves output excluded");
		     },
		     {false, false, false},
		     true},
		    {"unmuted_only",
		     [] {
			     click("studioMixerMute");
			     check(!obs_source_muted(music) &&
					   obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_MONITOR_ONLY,
				   "unmuting Monitor Only cannot reenable program audio");
			     action("actionMainUndo");
			     check(obs_source_muted(music) &&
					   obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_MONITOR_ONLY,
				   "single mute Undo retains Monitor Only");
			     action("actionMainRedo");
		     },
		     {false, false, false},
		     true},
		    {"protected_only",
		     [] {
			     const QString undo = child<QAction>(window, "actionMainUndo")->text();
			     click("studioMixerCue");
			     check(obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_MONITOR_ONLY &&
					   !obs_source_muted(music),
				   "CUE off is refused for Monitor Only without changing mute");
			     check(child<QPushButton>(strip, "studioMixerCue")->isChecked(),
				   "protected Monitor Only CUE stays checked");
			     check(child<QAction>(window, "actionMainUndo")->text() == undo,
				   "refused CUE off adds no undo action");
			     persist();
		     },
		     {false, false, false},
		     true},
		    {"delay_zero",
		     [] {
			     mode(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
			     delay(0);
		     },
		     {true, true, true},
		     true},
		    {"delay_250",
		     [] {
			     delay(250);
			     check(obs_source_get_sync_offset(music) == 250000000,
				   "250ms source delay reaches program backend");
			     persist();
		     },
		     {true, true, true},
		     true},
		    {"vod_excluded",
		     [] {
			     delay(0);
			     auto *b = child<QPushButton>(window, "studioAudioVodExclude");
			     check(b->property("studioVodMask").toUInt() == 2 &&
					   b->property("studioLiveMask").toUInt() == 1,
				   "VOD UI resolves explicit independent local clean and program mixes");
			     b->click();
			     check(obs_source_get_audio_mixers(music) == 5, "VOD exclusion changes only clean mix bit");
			     persist();
		     },
		     {true, false, true},
		     true},
		    {"vod_undo",
		     [] {
			     action("actionMainUndo");
			     check(obs_source_get_audio_mixers(music) == 7,
				   "one Undo restores VOD without touching recording or live mix");
		     },
		     {true, true, true},
		     true},
		    {"physical_monitor",
		     [] {
			     endpoint(physicalSink);
			     mode(OBS_MONITORING_TYPE_MONITOR_ONLY);
		     },
		     {false, false, false},
		     true,
		     true,
		     true},
		    {"physical_input",
		     [] {
			     mode(OBS_MONITORING_TYPE_NONE);
			     endpoint(virtualSink);
			     obs_source_set_audio_mixers(music, 0);
			     obs_source_set_audio_mixers(voice, 0);
			     obs_source_set_audio_mixers(reference, 0);
			     OBSDataAutoRelease d = obs_data_create();
			     obs_data_set_string(d, "device_id", micDevice.toUtf8().constData());
			     microphone =
				     obs_source_create("pulse_input_capture", "Audio QA physical input", d, nullptr);
			     check(bool(microphone), "explicit physical Pulse input source is created");
			     obs_source_set_audio_mixers(microphone, 8);
			     obs_source_set_monitoring_type(microphone, OBS_MONITORING_TYPE_NONE);
			     obs_scene_add(scene, microphone);
		     },
		     {false, false, false},
		     false,
		     false,
		     false,
		     true}};
}
void startRecording()
{
	obs_video_info vi{};
	check(obs_get_video_info(&vi) && vi.fps_den > 0, "actual video clock is available for file stage alignment");
	videoFps = double(vi.fps_num) / vi.fps_den;
	facts["video_fps"] = videoFps;
	OBSDataAutoRelease v = obs_data_create();
	obs_data_set_string(v, "rate_control", "CBR");
	obs_data_set_int(v, "bitrate", 200);
	obs_data_set_string(v, "preset", "ultrafast");
	video = obs_video_encoder_create("obs_x264", "audio-qa-video", v, nullptr);
	check(bool(video), "owned low-resolution video encoder created");
	obs_encoder_set_scaled_size(video, 320, 180);
	obs_encoder_set_video(video, obs_get_video());
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_string(d, "path", (root + "/paths.mkv").toUtf8().constData());
	recording = obs_output_create("ffmpeg_muxer", "audio-qa-private-multitrack", d, nullptr);
	check(bool(recording), "private local audio multitrack muxer created");
	obs_output_set_video_encoder(recording, video);
	audio_convert_info convert{};
	convert.samples_per_sec = 48000;
	convert.format = AUDIO_FORMAT_FLOAT_PLANAR;
	convert.speakers = SPEAKERS_STEREO;
	QJsonArray streams;
	for (size_t i = 0; i < 4; ++i) {
		OBSDataAutoRelease a = obs_data_create();
		obs_data_set_int(a, "bitrate", 160);
		encoders[i] = obs_audio_encoder_create(
			"ffmpeg_aac", QString("audio-qa-mix%1").arg(i).toUtf8().constData(), a, i, nullptr);
		check(bool(encoders[i]), "dedicated AAC encoder binds the intended mixer");
		obs_encoder_set_audio(encoders[i], obs_get_audio());
		obs_output_set_audio_encoder(recording, encoders[i], i);
		check(audio_output_connect(obs_get_audio(), i, &convert, capture, nullptr),
		      "native mixed PCM callback connected");
		connected.push_back(i);
		streams.append(QJsonObject{{"track", int(i)},
					   {"mixer", int(obs_encoder_get_mixer_index(encoders[i]))},
					   {"encoder", obs_encoder_get_id(encoders[i])}});
	}
	facts["encoded_audio_tracks"] = streams;
	check(obs_output_start(recording), "private multitrack recording starts with four real audio encoders");
	recordingStarted = true;
	prepareSteps();
	later(1200, next);
}
void setup()
{
	check(profile() == qaProfile, "only the dedicated temporary profile is active");
	auto *c = obs_frontend_get_profile_config();
	config_set_string(c, "Output", "Mode", "Advanced");
	config_set_int(c, "AdvOut", "TrackIndex", 1);
	config_set_int(c, "AdvOut", "VodTrackIndex", 2);
	config_set_bool(c, "AdvOut", "VodTrackEnabled", true);
	config_save_safe(c, "tmp", nullptr);
	std::string failure;
	check(manager()->Configure({}, failure), "QA VOD plan excludes all unrelated configured destinations");
	OBSDataAutoRelease service = obs_data_create();
	obs_data_set_string(service, "service", "Twitch");
	obs_data_set_string(service, "server", "rtmp://127.0.0.1:9/live");
	OBSServiceAutoRelease local = obs_service_create("rtmp_common", "audio-qa-no-network", service, nullptr);
	obs_frontend_set_streaming_service(local);
	for (size_t i = 0; i < globalAudio.size(); ++i) {
		globalAudio[i] = obs_get_output_source(unsigned(i + 1));
		obs_set_output_source(unsigned(i + 1), nullptr);
	}
	if (studioMode) {
		obs_frontend_set_preview_program_mode(false);
	}
	scene = obs_scene_create("Audio CUE VOD QA");
	music = obs_source_create("studio_audio_cue_qa", "Audio QA music 997Hz", nullptr, nullptr);
	voice = obs_source_create("studio_audio_cue_qa", "Audio QA voice 431Hz", nullptr, nullptr);
	reference = obs_source_create("studio_audio_cue_qa", "Audio QA recording reference 2003Hz", nullptr, nullptr);
	check(music && voice && reference && scene, "isolated synthetic tone scene exists");
	obs_source_set_audio_mixers(music, 7);
	obs_source_set_audio_mixers(voice, 7);
	obs_source_set_audio_mixers(reference, 4);
	obs_scene_add(scene, music);
	obs_scene_add(scene, voice);
	obs_scene_add(scene, reference);
	obs_frontend_set_current_scene(obs_scene_get_source(scene));
	startToneWorker({music.Get(), voice.Get(), reference.Get()});
	later(1200, [] {
		for (auto *w : window->findChildren<QWidget *>()) {
			if (QByteArray(w->metaObject()->className()) == "VolumeControl" &&
			    w->objectName() == "Audio QA music 997Hz") {
				strip = w;
			}
		}
		check(strip != nullptr, "actual mixer strip for synthetic input is available");
		check(QMetaObject::invokeMethod(window, "OpenStudioAudioRouting", Qt::DirectConnection,
						Q_ARG(QString, QString::fromUtf8(obs_source_get_uuid(music)))),
		      "actual audio dock focuses the test source");
		later(700, [] {
			endpoint(virtualSink);
			startRecording();
		});
	});
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "OBS is idle before CUE/VOD test");
	const auto expected = QFileInfo(configRoot).canonicalFilePath();
	check(!configRoot.isEmpty() && !expected.isEmpty(), "private configuration root is required");
	char *p = obs_frontend_get_current_profile_path();
	QString path = QFileInfo(QString::fromUtf8(p)).canonicalFilePath();
	bfree(p);
	check(path.startsWith(expected + "/"),
	      "effective profile lies inside private configuration root before any mutation");
	for (const char *k : {"Configuration", "SceneCollections", "Profiles", "PluginManagerSettings"}) {
		check(QFileInfo(QString::fromUtf8(config_get_string(obs_frontend_get_app_config(), "Locations", k)))
				      .canonicalFilePath() == expected,
		      "all four absolute Locations target the private root");
	}
	originalLayout = window->saveState();
	originalScene = obs_frontend_get_current_scene();
	originalSceneUuid = QString::fromUtf8(obs_source_get_uuid(originalScene));
	studioMode = obs_frontend_preview_program_mode_active();
	originalService = obs_frontend_get_streaming_service();
	originalChannels = manager()->ConfiguredChannels();
	originalProfile = profile();
	const char *n = nullptr, *id = nullptr;
	obs_get_audio_monitoring_device(&n, &id);
	monitorName = QString::fromUtf8(n);
	monitorId = QString::fromUtf8(id);
	ready = true;
	facts = QJsonObject{
		{"original_scene_uuid", originalSceneUuid},
		{"virtual_sink", virtualSink},
		{"physical_sink", physicalSink},
		{"physical_source", micDevice},
		{"hardware_tone_peak_dbfs", 20 * std::log10(.02)},
		{"no_public_network", true},
		{"vod_plan", "prepared local clean mix only; not Twitch ingest"},
		{"cue_delay_policy",
		 "Pulse monitor ignores source sync offset and muted callback by design; program offset measured independently"}};
	qaProfile = "Audio CUE QA " + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
	obs_frontend_duplicate_profile(qaProfile.toUtf8().constData());
	duplicated = true;
	later(600, setup);
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		stopToneWorker();
		return;
	}
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1500, run);
	} else if (e == OBS_FRONTEND_EVENT_EXIT) {
		stopToneWorker();
		for (auto &saved : globalAudio)
			saved = nullptr;
		originalScene = nullptr;
		originalService = nullptr;
	}
}
const char *name(void *)
{
	return "Audio CUE QA synthetic input";
}
void *create(obs_data_t *, obs_source_t *s)
{
	return s;
}
void destroy(void *) {}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_ROOT");
	configRoot = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_CONFIG_ROOT");
	virtualSink = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_SINK");
	physicalSink = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_PHYSICAL_SINK");
	micDevice = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_MIC");
	ffmpeg = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_FFMPEG", "/usr/bin/ffmpeg");
	if (root.isEmpty() || virtualSink.isEmpty() || physicalSink.isEmpty() || micDevice.isEmpty() ||
	    QFileInfo::exists(root + "/runtime.json") || !QDir().mkpath(root)) {
		return false;
	}
	obs_source_info i{};
	i.id = "studio_audio_cue_qa";
	i.type = OBS_SOURCE_TYPE_INPUT;
	i.output_flags = OBS_SOURCE_AUDIO;
	i.get_name = name;
	i.create = create;
	i.destroy = destroy;
	obs_register_source(&i);
	obs_frontend_add_event_callback(event, nullptr);
	QTimer::singleShot(150000, qApp, [] {
		if (!done) {
			finish("audio fixture exceeded 150 second deadline");
		}
	});
	return true;
}
void obs_module_unload()
{
	stopToneWorker();
	obs_frontend_remove_event_callback(event, nullptr);
}
