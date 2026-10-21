/* Separate Windows-native CUE/VOD probe; Linux historical fixture is unchanged.
 * The parent owns OBS/VM lifecycle and endpoint classification. No defaults or
 * endpoint gains are changed. Optional physical input is private, never sent
 * to network or monitoring. HDA in the VM is virtual, not physical validation.
 * A/V async source frames deliberately exercise WASAPI's video-synchronized
 * monitoring branch; audio-only sources do not use that delay branch. */
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
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	error = "Audio fixture never persists destinations";
	return false;
}
namespace {
QString root, configRoot, virtualSink, physicalSink, micDevice, captureTool, endpointClass;
QString originalProfile, qaProfile, monitorName, monitorId, originalSceneUuid;
QMainWindow *window = nullptr;
QWidget *strip = nullptr;
QJsonArray checks, stages;
QJsonObject facts;
QByteArray originalLayout;
bool done = false, duplicated = false, ready = false, recordingStarted = false, studioMode = false,
     globalsCaptured = false;
QJsonArray skipped;
int stageIndex = 0;
std::thread toneWorker;
std::atomic<bool> toneStop{false}, toneFailed{false};
std::atomic<uint64_t> toneFrames{0}, toneBlocks{0}, toneLateBlocks{0}, toneCatchupBlocks{0},
	toneMaxLatenessNs{0}, toneMaxDispatchNs{0};
QProcess *captureProcess = nullptr;
OBSSceneAutoRelease scene;
OBSSourceAutoRelease music, voice, reference, pilot, microphone, originalScene;
std::array<OBSSourceAutoRelease, 5> globalAudio;
OBSService originalService;
std::vector<MultiStreamChannel> originalChannels;
OBSOutputAutoRelease recording;
OBSEncoderAutoRelease video;
std::array<OBSEncoderAutoRelease, 4> encoders;
std::array<std::unique_ptr<QFile>, 4> pcm;
std::array<uint64_t, 4> frameCounts{};
std::mutex pcmMutex;
bool capturePcm = false;
bool shuttingDown = false;
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
	bool monitorReference = false;
};
std::vector<Stage> sequence;
QJsonObject currentStage;
void finish(QString error = {});
void atomicMax(std::atomic<uint64_t> &value, uint64_t candidate)
{
	auto previous = value.load();
	while (candidate > previous && !value.compare_exchange_weak(previous, candidate)) {
	}
}
QJsonObject toneMetrics()
{
	return {{"frames_per_source", qint64(toneFrames.load())}, {"blocks", qint64(toneBlocks.load())},
		{"late_blocks_over_1ms", qint64(toneLateBlocks.load())},
		{"catchup_blocks", qint64(toneCatchupBlocks.load())},
		{"max_lateness_ms", double(toneMaxLatenessNs.load()) / 1000000.},
		{"max_dispatch_ms", double(toneMaxDispatchNs.load()) / 1000000.},
		{"backlog_failed", toneFailed.load()}, {"max_lateness_bound_ms", 100},
		{"max_consecutive_catchup_blocks", 3},
		{"clock", "one monotonic anchor plus exact sample count; shared audio/video timestamp per block"}};
}
void stopToneWorker()
{
	toneStop.store(true);
	if (toneWorker.joinable()) {
		toneWorker.join();
	}
}
void pollToneWorker()
{
	if (done) {
		return;
	}
	if (toneFailed.load()) {
		finish("synthetic source worker exceeded bounded clock backlog");
		return;
	}
	QTimer::singleShot(100, qApp, pollToneWorker);
}
void startToneWorker(std::array<obs_source_t *, 4> inputs)
{
	toneWorker = std::thread([inputs] {
		// These owning refs remain local to the worker; UI cleanup joins before
		// removing sources. No GUI objects or report files are touched here.
		std::array<OBSSourceAutoRelease, 4> sources;
		for (size_t i = 0; i < sources.size(); ++i) {
			sources[i] = obs_source_get_ref(inputs[i]);
		}
		std::array<uint8_t, 32 * 18 * 4> pixels{};
		for (size_t i = 3; i < pixels.size(); i += 4) {
			pixels[i] = 255;
		}
		const uint64_t anchor = os_gettime_ns();
		uint64_t sampleIndex = 0;
		unsigned consecutiveCatchups = 0;
		while (!toneStop.load()) {
			const uint64_t timestamp = anchor + sampleIndex * 1000000000ULL / 48000;
			os_sleepto_ns_fast(timestamp);
			if (toneStop.load()) {
				break;
			}
			const uint64_t dispatchStart = os_gettime_ns();
			const uint64_t late = dispatchStart > timestamp ? dispatchStart - timestamp : 0;
			atomicMax(toneMaxLatenessNs, late);
			if (late > 1000000) {
				toneLateBlocks.fetch_add(1);
			}
			if (late >= 20000000) {
				toneCatchupBlocks.fetch_add(1);
				++consecutiveCatchups;
			} else {
				consecutiveCatchups = 0;
			}
			if (late > 100000000 || consecutiveCatchups > 3) {
				// Never drop, retimestamp or insert silence to hide a stalled feed.
				toneFailed.store(true);
				break;
			}
			std::array<std::array<float, 960>, 4> samples;
			for (size_t i = 0; i < 960; ++i) {
				const double t = double(sampleIndex + i) / 48000.;
				const bool on = ((sampleIndex + i) % 24000) < 9600;
				samples[0][i] = on * .010 * std::sin(2 * 3.141592653589793 * 997 * t);
				samples[1][i] = .015 * std::sin(2 * 3.141592653589793 * 431 * t);
				samples[2][i] = on * .006 * std::sin(2 * 3.141592653589793 * 2003 * t);
				samples[3][i] = .003 * std::sin(2 * 3.141592653589793 * 3037 * t);
			}
			obs_source_audio audio{};
			audio.frames = 960;
			audio.samples_per_sec = 48000;
			audio.speakers = SPEAKERS_MONO;
			audio.format = AUDIO_FORMAT_FLOAT;
			audio.timestamp = timestamp;
			obs_source_frame frame{};
			frame.format = VIDEO_FORMAT_RGBA;
			frame.width = 32;
			frame.height = 18;
			frame.linesize[0] = 32 * 4;
			frame.data[0] = pixels.data();
			frame.timestamp = timestamp;
			frame.full_range = true;
			for (size_t i = 0; i < sources.size(); ++i) {
				obs_source_output_video(sources[i], &frame);
				audio.data[0] = reinterpret_cast<uint8_t *>(samples[i].data());
				obs_source_output_audio(sources[i], &audio);
			}
			atomicMax(toneMaxDispatchNs, os_gettime_ns() - dispatchStart);
			sampleIndex += 960;
			toneFrames.store(sampleIndex);
			toneBlocks.fetch_add(1);
		}
	});
	QTimer::singleShot(100, qApp, pollToneWorker);
}
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
void writeReport(QString error)
{
	facts["tone_worker"] = toneMetrics();
	QSaveFile f(root + "/runtime.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"stages", stages},
					{"facts", facts},
					{"linux_native", false},
#if defined(Q_OS_WIN)
					{"windows_target", true},
#else
					{"windows_target", false},
#endif
					{"skipped", skipped},
					{"runtime_environment_verified", false},
					{"runtime_provenance",
					 "Q_OS_WIN identifies target only; parent supplies external VM/native evidence"},
					{"scope",
					 "Actual Qt controls, libobs mixed PCM and four locally encoded AAC mixes, explicit WASAPI endpoint loopback. Endpoint class and omitted optional physical cases are explicit. A/V async source delay path; no public streaming, acoustic latency or Twitch ingest certification."}})
				.toJson());
		f.commit();
	}
}
void restore(QString error, std::shared_ptr<QElapsedTimer> clock, bool forcedStop = false)
{
	if (shuttingDown) {
		return;
	}
	if (recording && obs_output_active(recording)) {
		if (clock->elapsed() > (forcedStop ? 10000 : 15000)) {
			if (!forcedStop && recordingStarted) {
				error += "; output stop deadline, forced owned output stop";
				facts["forced_owned_output_stop"] = QJsonObject{
					{"requested", true}, {"normal_stop_deadline_ms", 15000},
					{"force_stop_deadline_ms", 10000}, {"completed", false}};
				// Keep the first failure durable even if the forced teardown also stalls.
				writeReport(error);
				obs_output_force_stop(recording);
				clock->restart();
				QTimer::singleShot(100, qApp, [error, clock] { restore(error, clock, true); });
			} else {
				writeReport(error + "; output still active, private fixture retained");
			}
			return;
		}
		QTimer::singleShot(100, qApp, [error, clock, forcedStop] { restore(error, clock, forcedStop); });
		return;
	}
	// Feed remains alive through normal/forced stop. Join before changing the
	// endpoint or releasing any source reference used by the worker.
	stopToneWorker();
	facts["tone_worker"] = toneMetrics();
	if (toneFailed.load()) {
		error += "; synthetic source worker exceeded bounded clock backlog";
	}
	if (forcedStop) {
		auto result = facts["forced_owned_output_stop"].toObject();
		result["completed"] = true;
		facts["forced_owned_output_stop"] = result;
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
	for (auto *testSource : {music.Get(), reference.Get(), pilot.Get()}) {
		if (testSource) {
			obs_source_set_monitoring_type(testSource, OBS_MONITORING_TYPE_NONE);
		}
	}
	obs_set_audio_monitoring_device(monitorName.toUtf8().constData(), monitorId.toUtf8().constData());
	if (originalScene) {
		obs_frontend_set_current_scene(originalScene);
	}
	if (globalsCaptured) {
		for (size_t i = 0; i < globalAudio.size(); ++i) {
			obs_set_output_source(unsigned(i + 1), globalAudio[i]);
			globalAudio[i] = nullptr;
		}
	}
	for (auto *s : {microphone.Get(), music.Get(), voice.Get(), reference.Get(), pilot.Get(),
			scene ? obs_scene_get_source(scene) : nullptr}) {
		if (s) {
			obs_source_remove(s);
		}
	}
	microphone = nullptr;
	music = nullptr;
	voice = nullptr;
	reference = nullptr;
	pilot = nullptr;
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
	writeReport(error);
}
void finish(QString error)
{
	if (done) {
		return;
	}
	done = true;
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
		check(clock->elapsed() < 9000, "explicit WASAPI endpoint capture completes before deadline");
		later(100, [clock] { finishStage(clock); });
		return;
	}
	QFile log(root + "/" + sequence[stageIndex].name + "-capture.log");
	if (log.open(QIODevice::WriteOnly)) {
		log.write(captureProcess->readAllStandardError());
	}
	check(captureProcess->exitStatus() == QProcess::NormalExit && captureProcess->exitCode() == 0,
	      "finite WASAPI capture from the explicitly selected endpoint succeeds");
	const auto captureStats = QJsonDocument::fromJson(captureProcess->readAllStandardOutput()).object();
	check(captureStats["captured"].toBool(), "native endpoint helper reports a completed capture");
	check(captureStats["sample_rate"].toDouble() > 0 &&
		      captureStats["written_frames"].toDouble() >= captureStats["sample_rate"].toDouble() &&
		      captureStats["received_frames"].toDouble() >= captureStats["sample_rate"].toDouble(),
	      "every endpoint case has actual received and written frames, including negative controls");
	check(captureStats["device_id"].toString() == virtualSink,
	      "capture helper used the exact explicit monitoring endpoint ID");
	check(obs_source_get_audio_mixers(pilot) == 0 &&
		      obs_source_get_monitoring_type(pilot) == OBS_MONITORING_TYPE_MONITOR_ONLY &&
		      !obs_source_muted(pilot),
	      "permanent monitoring pilot stays monitor-only with no program mix");
	currentStage["endpoint_capture"] = captureStats;
	currentStage["tone_worker_end"] = toneMetrics();
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
	currentStage = QJsonObject{{"tone_worker_begin", toneMetrics()}, {"name", s.name},
				   {"expected_music", QJsonArray{s.music[0], s.music[1], s.music[2]}},
				   {"expected_monitor_music", s.monitor},
				   {"expected_voice", s.voice},
				   {"physical_endpoint", endpointClass == "physical"},
				   {"endpoint_class", endpointClass},
				   {"endpoint_id", virtualSink},
				   {"expected_monitor_pilot", true},
				   {"expected_monitor_reference", s.monitorReference},
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
	captureProcess->setProgram(captureTool);
	captureProcess->setWorkingDirectory(root);
	captureProcess->setArguments(
		{"--device-id", virtualSink, "--seconds", "3", "--output", root + "/" + s.name + "-monitor.wav"});
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
			     click("studioMixerCue");
			     check(obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_NONE,
				   "actual CUE OFF click changes BOTH to NONE");
			     action("actionMainUndo");
			     check(obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
				   "single Undo restores BOTH after actual OFF click");
			     action("actionMainRedo");
			     check(obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_NONE,
				   "Redo reapplies actual CUE OFF click");
			     click("studioMixerCue");
			     check(obs_source_get_monitoring_type(music) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
				   "CUE ON click restores measured BOTH state");
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
		    {"endpoint_monitor",
		     [] {
			     obs_source_set_monitoring_type(reference, OBS_MONITORING_TYPE_NONE);
			     delay(0);
			     endpoint(virtualSink);
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
				     obs_source_create("wasapi_input_capture", "Audio QA physical input", d, nullptr);
			     check(bool(microphone), "explicit optional WASAPI input source is created");
			     obs_source_set_audio_mixers(microphone, 8);
			     obs_source_set_monitoring_type(microphone, OBS_MONITORING_TYPE_NONE);
			     obs_scene_add(scene, microphone);
		     },
		     {false, false, false},
		     false,
		     false,
		     false,
		     true}};
	if (micDevice.isEmpty()) {
		sequence.pop_back();
		skipped.append(QJsonObject{
			{"case", "physical_input"},
			{"reason",
			 "No explicit microphone device ID supplied; VM has no physical input; not validated"}});
	}
	if (endpointClass != "physical") {
		skipped.append(QJsonObject{
			{"case", "physical_endpoint_hardware"},
			{"reason",
			 "Selected endpoint is runner-classified virtual HDA; rendered digital endpoint samples do not certify physical hardware"}});
	}
	Stage zero{"cue_delay_zero",
		   [] {
			   mode(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
			   delay(0);
			   obs_source_set_monitoring_type(reference, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
		   },
		   {true, true, true},
		   true,
		   true,
		   false,
		   false,
		   true};
	Stage offset{"cue_delay_250", [] {delay(250); }, {true, true, true}, true, true, false, false, true};
	sequence.insert(sequence.begin() + 12, zero);
	sequence.insert(sequence.begin() + 13, offset);
}
void startRecording()
{
	check(obs_source_get_width(music) == 32 && obs_source_get_height(music) == 18,
	      "async video frames actually reach the monitored A/V source");
	facts["async_source_dimensions"] =
		QJsonArray{int(obs_source_get_width(music)), int(obs_source_get_height(music))};
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
	config_set_uint(c, "Video", "BaseCX", 640);
	config_set_uint(c, "Video", "BaseCY", 360);
	config_set_uint(c, "Video", "OutputCX", 640);
	config_set_uint(c, "Video", "OutputCY", 360);
	config_set_uint(c, "Video", "FPSType", 0);
	config_set_string(c, "Video", "FPSCommon", "30");
	obs_frontend_reset_video();
	obs_video_info vi{};
	check(obs_get_video_info(&vi) && vi.base_width == 640 && vi.base_height == 360 && vi.output_width == 640 &&
		      vi.output_height == 360 && vi.fps_num == 30 * vi.fps_den,
	      "QA video is actually 640x360 at 30fps for CPU/WARP");
	obs_audio_info ai{};
	check(obs_get_audio_info(&ai) && ai.samples_per_sec == 48000,
	      "Windows audio engine runs at explicit 48kHz precondition");
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
	globalsCaptured = true;
	if (studioMode) {
		obs_frontend_set_preview_program_mode(false);
	}
	scene = obs_scene_create("Audio CUE VOD QA");
	music = obs_source_create("studio_audio_cue_windows_qa", "Audio QA music 997Hz", nullptr, nullptr);
	voice = obs_source_create("studio_audio_cue_windows_qa", "Audio QA voice 431Hz", nullptr, nullptr);
	reference = obs_source_create("studio_audio_cue_windows_qa", "Audio QA recording reference 2003Hz", nullptr,
				      nullptr);
	pilot = obs_source_create("studio_audio_cue_windows_qa", "Audio QA monitoring pilot 3037Hz", nullptr, nullptr);
	check(music && voice && reference && pilot && scene,
	      "isolated synthetic tone scene and permanent monitoring pilot exist");
	obs_source_set_audio_mixers(pilot, 0);
	obs_scene_add(scene, pilot);
	obs_source_set_audio_mixers(music, 7);
	obs_source_set_audio_mixers(voice, 7);
	obs_source_set_audio_mixers(reference, 4);
	obs_scene_add(scene, music);
	obs_scene_add(scene, voice);
	obs_scene_add(scene, reference);
	obs_frontend_set_current_scene(obs_scene_get_source(scene));
	startToneWorker({music.Get(), voice.Get(), reference.Get(), pilot.Get()});
	later(1200, [] {
		for (auto *w : window->findChildren<QWidget *>()) {
			if (QByteArray(w->metaObject()->className()) == "VolumeControl" &&
			    w->objectName() == "Audio QA music 997Hz" && w->property("studioStrip").toBool()) {
				strip = w;
			}
		}
		check(strip != nullptr, "actual mixer strip for synthetic input is available");
		check(strip->property("studioStrip").toBool(), "source uses the Studio strip behavior under test");
		check(QMetaObject::invokeMethod(window, "OpenStudioAudioRouting", Qt::DirectConnection,
						Q_ARG(QString, QString::fromUtf8(obs_source_get_uuid(music)))),
		      "actual audio dock focuses the test source");
		later(700, [] {
			endpoint(virtualSink);
			obs_source_set_monitoring_type(pilot, OBS_MONITORING_TYPE_MONITOR_ONLY);
			startRecording();
		});
	});
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "OBS is idle before CUE/VOD test");
	check(physicalSink.isEmpty(),
	      "one explicit endpoint per run; alternate physical sink requires a separate classified run");
	const auto expected = QFileInfo(configRoot).canonicalFilePath();
	check(!configRoot.isEmpty() && !expected.isEmpty(), "private configuration root is required");
	char *p = obs_frontend_get_current_profile_path();
	QString path = QFileInfo(QString::fromUtf8(p)).canonicalFilePath();
	bfree(p);
	check(path.startsWith(expected + "/"),
	      "effective profile lies inside private configuration root before any mutation");
	QJsonObject locations;
	for (const char *k : {"Configuration", "SceneCollections", "Profiles", "PluginManagerSettings"}) {
		auto *app = obs_frontend_get_app_config();
		if (config_has_user_value(app, "Locations", k)) {
			const QString raw = QString::fromUtf8(config_get_string(app, "Locations", k));
			locations[k] = raw;
			check(!raw.isEmpty() && QFileInfo(raw).canonicalFilePath() == expected,
			      "explicit portable Locations value targets the private root");
		} else {
			locations[k] = "absent: portable default; effective profile guard still required";
		}
	}
	check(config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "StudioWorkspace"),
	      "Studio workspace must be enabled in the isolated portable fixture");
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
		{"target_endpoint", virtualSink},
		{"endpoint_class", endpointClass},
		{"locations", locations},
		{"source_kind", "A/V async frame source exercises WASAPI video-synchronized monitoring path"},
		{"physical_sink", physicalSink},
		{"physical_source", micDevice},
		{"synthetic_music_source_peak_dbfs", 20 * std::log10(.010)},
		{"monitoring_pilot_source_peak_dbfs", 20 * std::log10(.003)},
		{"theoretical_combined_monitor_mono_peak_dbfs", 20 * std::log10((.010 + .006 + .003) * std::sqrt(2.))},
		{"monitoring_pilot_hz", 3037},
		{"input_identity", "Explicit optional input ID does not by itself certify physical hardware"},
		{"no_public_network", true},
		{"vod_plan", "prepared local clean mix only; not Twitch ingest"},
		{"cue_delay_policy",
		 "WASAPI mute callback remains independent; sync_offset is gated by source_has_video and !async_decoupled. This A/V source tests program and endpoint pulse delay separately; no acoustic latency claim"}};
	qaProfile = "Audio CUE QA " + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
	obs_frontend_duplicate_profile(qaProfile.toUtf8().constData());
	duplicated = true;
	later(600, setup);
}
void shutdownFixture()
{
	if (shuttingDown) {
		return;
	}
	shuttingDown = true;
	done = true;
	stopToneWorker(); // SCRIPTING_SHUTDOWN is before frontend ClearSceneData.
	if (captureProcess && captureProcess->state() != QProcess::NotRunning) {
		captureProcess->kill();
		captureProcess->waitForFinished(1500);
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
	const bool idle = !recording || !obs_output_active(recording);
	if (idle) {
		for (auto mix : connected) {
			audio_output_disconnect(obs_get_audio(), mix, capture, nullptr);
		}
		connected.clear();
		recording = nullptr;
		video = nullptr;
		for (auto &e : encoders) {
			e = nullptr;
		}
		for (auto *s : {microphone.Get(), music.Get(), voice.Get(), reference.Get(), pilot.Get(),
				scene ? obs_scene_get_source(scene) : nullptr}) {
			if (s) {
				obs_source_remove(s);
			}
		}
		microphone = nullptr;
		music = nullptr;
		voice = nullptr;
		reference = nullptr;
		pilot = nullptr;
		scene = nullptr;
		for (auto &g : globalAudio) {
			g = nullptr;
		}
		originalScene = nullptr;
		originalService = nullptr;
		ready = false;
	}
	QSaveFile f(root + "/shutdown.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"boundary", "SCRIPTING_SHUTDOWN before ClearSceneData"},
			{"owned_output_idle", idle}, {"owned_references_released", idle},
			{"profile_or_UI_restored", false}, {"runtime_result_unchanged", true},
			{"tone_worker", toneMetrics()}}).toJson());
		f.commit();
	}
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		shutdownFixture();
		return;
	}
	if (e == OBS_FRONTEND_EVENT_EXIT) {
		stopToneWorker();
	}
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1500, run);
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
#if !defined(Q_OS_WIN)
	return false; // Build-only API check on other systems; never claim a Windows run.
#endif
	root = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_ROOT");
	configRoot = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_CONFIG_ROOT");
	virtualSink = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_SINK");
	physicalSink = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_PHYSICAL_SINK");
	micDevice = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_MIC");
	captureTool = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_CAPTURE_TOOL");
	endpointClass = qEnvironmentVariable("OBS_STUDIO_CUE_VOD_ENDPOINT_CLASS");
	if (root.isEmpty() || virtualSink.isEmpty() || captureTool.isEmpty() || !QFileInfo(captureTool).isFile() ||
	    (endpointClass != "physical" && endpointClass != "virtual") || QFileInfo::exists(root + "/runtime.json") ||
	    !QDir().mkpath(root)) {
		return false;
	}
	obs_source_info i{};
	i.id = "studio_audio_cue_windows_qa";
	i.type = OBS_SOURCE_TYPE_INPUT;
	i.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC_VIDEO;
	i.get_name = name;
	i.create = create;
	i.destroy = destroy;
	obs_register_source(&i);
	obs_frontend_add_event_callback(event, nullptr);
	QTimer::singleShot(180000, qApp, [] {
		if (!done) {
			finish("audio fixture exceeded 180 second deadline");
		}
	});
	return true;
}
void obs_module_unload()
{
	stopToneWorker();
	obs_frontend_remove_event_callback(event, nullptr);
}
