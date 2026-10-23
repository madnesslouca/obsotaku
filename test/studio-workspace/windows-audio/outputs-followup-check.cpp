/* Windows WARP follow-up: phase 0 only at Main640x36015 and Portrait1080x192015.
 * Derived from the shared dual-replay integration contract.
 * Opt-in native frontend integration: the real app's stream manager, Record,
 * Replay Save and Stop/Pause APIs run together against loopback RTMP receivers.
 * A temporary duplicate profile keeps output/encoder settings isolated. */
#include <obs-module.h>
#include <obs-hotkey.h>
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
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
#include <array>
#include <atomic>
#include <chrono>
#include <util/platform.h>
#ifdef STUDIO_CLOCK_PRIVATE_OBSERVER
#include "output-clock-observer.h"
#endif
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	error = "The dual-replay fixture cannot persist destinations";
	return false;
}
namespace {
// Parent installs exactly this opt-in module after all production targets build.
// Every server URL is constructed from QHostAddress::LocalHost and an allocated
// local port. No account store, OAuth or public destination is read by the fixture.
QString root, server, originalProfile, qaProfile;
QMainWindow *window;
QJsonArray checks, files, plans, saves;
QTimer *payloadTimer = nullptr;
bool payloadViolation = false;
int stoppedZeroSamples = 0, pendingSaveAssertions = 0;
int completedSaveGroups[4] = {};
bool normalStopSaveCovered = false;
bool hotkeyExercised = false;
char mainTag[] = "main", portraitTag[] = "portrait";
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
bool shuttingDown = false, cleanupDeadlineMissed = false, diagnosticHoldApplied = false;
bool privateClockSampling = false;
bool restoring = false;
bool preflightReady = false;
QString firstFailure;
QElapsedTimer startupClock;
uint64_t bytesBefore[3] = {};
struct UserValue {
	const char *key;
	bool exists;
	QString value;
};
std::vector<UserValue> userValues;
struct ManagedSnapshot {
	OBSSource source;
	OBSData settings, privateSettings;
	int64_t time = 0;
	obs_media_state state = OBS_MEDIA_STATE_NONE;
};
std::vector<ManagedSnapshot> managed;
void preserveManagedSources()
{
	obs_enum_sources(
		[](void *, obs_source_t *source) {
			OBSDataAutoRelease priv = obs_source_get_private_settings(source);
			if (!obs_data_get_bool(priv, "studio_replay_managed")) {
				return true;
			}
			OBSDataAutoRelease settings = obs_source_get_settings(source);
			ManagedSnapshot item;
			item.source = source;
			item.settings = OBSDataAutoRelease(obs_data_create_from_json(obs_data_get_json(settings)));
			item.privateSettings = OBSDataAutoRelease(obs_data_create_from_json(obs_data_get_json(priv)));
			item.time = obs_source_media_get_time(source);
			item.state = obs_source_media_get_state(source);
			managed.push_back(std::move(item));
			/* Prevent QA saves from selecting a new file in the user's follow layer.
		 * Restore its complete private metadata after every asynchronous output stops. */
			obs_data_set_bool(priv, "studio_replay_follow", false);
			return true;
		},
		nullptr);
}
void restoreManagedSources()
{
	for (const auto &item : managed) {
		if (obs_source_removed(item.source)) {
			continue;
		}
		OBSDataAutoRelease current = obs_source_get_settings(item.source);
		if (QByteArray(obs_data_get_json(current)) != QByteArray(obs_data_get_json(item.settings))) {
			obs_source_reset_settings(item.source, item.settings);
			obs_source_media_set_time(item.source, item.time);
			if (item.state == OBS_MEDIA_STATE_PAUSED) {
				obs_source_media_play_pause(item.source, true);
			}
		}
		OBSDataAutoRelease priv = obs_source_get_private_settings(item.source);
		obs_data_clear(priv);
		obs_data_apply(priv, item.privateSettings);
	}
	managed.clear();
}
void finish(QString error = {});
void check(bool pass, const char *name)
{
	auto *activeModal = qApp->activeModalWidget();
	checks.append(QJsonObject{{"check", name},
				  {"passed", pass},
				  {"phase", phase},
				  {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
				  {"modal_class", activeModal ? activeModal->metaObject()->className() : ""},
				  {"modal_object", activeModal ? activeModal->objectName() : QString()},
				  {"modal_title", activeModal ? activeModal->windowTitle() : QString()}});
	if (!pass && firstFailure.isEmpty()) {
		firstFailure = QString::fromUtf8(name);
	}
	QSaveFile progress(root + (restoring ? "/restore-progress.json" : "/progress.json"));
	if (progress.open(QIODevice::WriteOnly)) {
		auto *modal = qApp->activeModalWidget();
		progress.write(QJsonDocument(QJsonObject{{"checks", checks},
							 {"phase", phase},
							 {"last_check", name},
							 {"passed_so_far", firstFailure.isEmpty()},
							 {"first_failure", firstFailure},
							 {"modal_class", modal ? modal->metaObject()->className() : ""},
							 {"modal_object", modal ? modal->objectName() : QString()},
							 {"modal_title", modal ? modal->windowTitle() : QString()}})
				       .toJson());
		progress.commit();
	}
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
		if (entry.id == "dual-replay-local-b") {
			return std::move(entry.output);
		}
	}
	return {};
}
OBSOutputAutoRelease portraitReplay()
{
	return obs_get_output_by_name("studio_portrait_replay");
}
OBSOutputAutoRelease mainReplay()
{
	return obs_get_output_by_name("studio_main_replay");
}
OBSOutputAutoRelease companion()
{
	return obs_get_output_by_name("studio_portrait_recording");
}
struct PacketEvent {
 uint64_t monotonicNs = 0, unixMs = 0;
 int64_t dts = 0, pts = 0, dtsUsec = 0, sysDtsUsec = 0;
 uint64_t cts = 0, fer = 0, ferc = 0, pir = 0;
 bool hasPacketTime = false;
 int32_t timebaseNum = 0, timebaseDen = 0;
 uint32_t kind = 0, type = 0, track = 0, generation = 0;
 uint64_t bytes = 0;
 bool keyframe = false;
};
struct PacketTap {
 OBSOutput output;
 std::array<PacketEvent,4096> events{};
 std::atomic<size_t> count{0}, overflow{0};
 std::atomic<unsigned> generation{0};
};
std::array<PacketTap,2> packetTaps;
QTimer *packetTapTimer = nullptr, *encoderPollTimer = nullptr;
QJsonArray encoderPolls;
size_t encoderPollOverflow = 0;
void tapEvent(PacketTap &tap, unsigned kind, const encoder_packet *packet = nullptr, const encoder_packet_time *time = nullptr)
{
 // Core serializes each output's packet callback. Signal callbacks can be on
 // another thread, so only packet records use this fixed append-only array.
 const auto i = tap.count.load(std::memory_order_relaxed);
 if(i>=tap.events.size()){tap.overflow.fetch_add(1);return;}
 auto &e=tap.events[i];e.monotonicNs=os_gettime_ns();
 e.unixMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
 e.kind=kind;e.generation=tap.generation.load();
 if(packet){e.type=packet->type;e.track=packet->track_idx;e.dts=packet->dts;e.pts=packet->pts;e.dtsUsec=packet->dts_usec;e.sysDtsUsec=packet->sys_dts_usec;e.timebaseNum=packet->timebase_num;e.timebaseDen=packet->timebase_den;e.bytes=packet->size;e.keyframe=packet->keyframe;}
 if(time){e.hasPacketTime=true;e.cts=time->cts;e.fer=time->fer;e.ferc=time->ferc;e.pir=time->pir;}
 tap.count.store(i+1,std::memory_order_release);
}
void packetTap(obs_output_t *, encoder_packet *packet, encoder_packet_time *time, void *opaque)
{
 tapEvent(*static_cast<PacketTap *>(opaque),1,packet,time);
}
void packetReconnect(void *opaque,calldata_t *)
{
 static_cast<PacketTap *>(opaque)->generation.fetch_add(1);
}
void detachPacketTaps()
{
 if(packetTapTimer)packetTapTimer->stop();
 if(encoderPollTimer)encoderPollTimer->stop();
 for(auto &tap:packetTaps){
  if(tap.output){obs_output_remove_packet_callback(tap.output,packetTap,&tap);signal_handler_disconnect(obs_output_get_signal_handler(tap.output),"reconnect",packetReconnect,&tap);}
  tap.output=nullptr;
 }
}
void attachPacketTaps()
{
 if(shuttingDown)return;
 const char *names[]={"simple_stream","multistream_output_dual-replay-local-b"};
 for(size_t i=0;i<2;++i){
  OBSOutputAutoRelease output=obs_get_output_by_name(names[i]);auto &tap=packetTaps[i];
  if(output && output!=tap.output){
   if(tap.output){obs_output_remove_packet_callback(tap.output,packetTap,&tap);signal_handler_disconnect(obs_output_get_signal_handler(tap.output),"reconnect",packetReconnect,&tap);}
   tap.output=output;tap.generation.fetch_add(1);
   obs_output_add_packet_callback(output,packetTap,&tap);
   signal_handler_connect(obs_output_get_signal_handler(output),"reconnect",packetReconnect,&tap);
  }
 }
}
#ifdef STUDIO_CLOCK_PRIVATE_OBSERVER
QJsonObject observePrivateClock(obs_output_t *output)
{
 // GUI lifecycle actions are serialized; pin the attached encoders as well as
 // the caller's output. Reconnect reuses these slots. Stop/configure disables
 // this observer before issuing lifecycle actions. Encoder fields remain racy
 // observations; queue fields alone are synchronized by the C helper.
 OBSEncoderAutoRelease video=obs_encoder_get_ref(obs_output_get_video_encoder(output));
 OBSEncoderAutoRelease audio=obs_encoder_get_ref(obs_output_get_audio_encoder(output,0));
 studio_output_clock_snapshot snap{};const int result=studio_output_clock_observe(output,&snap);
 auto packet=[](const studio_packet_clock_sample &p){return QJsonObject{{"present",bool(p.present)},{"keyframe",bool(p.keyframe)},{"dts",qint64(p.dts)},{"pts",qint64(p.pts)},{"dts_usec",qint64(p.dts_usec)},{"sys_dts_usec",qint64(p.sys_dts_usec)},{"timebase_num",p.timebase_num},{"timebase_den",p.timebase_den},{"track",qint64(p.track_index)}};};
 auto encoder=[](const studio_encoder_clock_sample &e){return QJsonObject{{"present",bool(e.present)},{"observational_only",bool(e.observational_only)},{"active",bool(e.active)},{"first_received",bool(e.first_received)},{"start_ts_ns",qint64(e.start_ts_ns)},{"first_raw_ts_ns",qint64(e.first_raw_ts_ns)},{"cur_pts",qint64(e.cur_pts)},{"timebase_num",int(e.timebase_num)},{"timebase_den",int(e.timebase_den)},{"audio_input_buffer0_bytes",qint64(e.audio_input_buffer0_bytes)},{"frame_size_samples",qint64(e.frame_size_samples)},{"sample_rate",int(e.sample_rate)}};};
 return QJsonObject{{"result",result},{"valid",bool(snap.valid)},{"lock_result",snap.lock_result},{"unlock_result",snap.unlock_result},{"abi_version",int(snap.abi_version)},{"snapshot_bytes",int(snap.snapshot_bytes)},{"internal_output_bytes",int(snap.internal_output_bytes)},{"internal_encoder_bytes",int(snap.internal_encoder_bytes)},{"sampled_at_ns",qint64(snap.sampled_at_ns)},{"encoder_fields_synchronized",false},{"packet_count",qint64(snap.packet_count)},{"received_audio",bool(snap.received_audio)},{"received_video0",bool(snap.received_video0)},{"highest_audio_ts",qint64(snap.highest_audio_ts)},{"highest_video_ts0",qint64(snap.highest_video_ts0)},{"video_offset0",qint64(snap.video_offset0)},{"audio_offset0",qint64(snap.audio_offset0)},{"attached_video_encoders",int(snap.attached_video_encoders)},{"attached_audio_encoders",int(snap.attached_audio_encoders)},{"first_video0",packet(snap.first_video0)},{"last_video0",packet(snap.last_video0)},{"first_audio0",packet(snap.first_audio0)},{"last_audio0",packet(snap.last_audio0)},{"video_encoder0",encoder(snap.video_encoder0)},{"audio_encoder0",encoder(snap.audio_encoder0)}};
}
#endif
void pollEncoders()
{
 if(shuttingDown)return;
 if(encoderPolls.size()>=1024){++encoderPollOverflow;return;}
 QJsonArray outputs;
 const char *names[]={"simple_stream","multistream_output_dual-replay-local-b"};
 for(size_t i=0;i<2;++i){
  OBSOutputAutoRelease output=obs_get_output_by_name(names[i]);
  if(!output)continue;
  auto *video=obs_output_get_video_encoder(output);auto *audio=obs_output_get_audio_encoder(output,0);
  OBSDataAutoRelease settings=obs_output_get_settings(output);
  auto encoder=[](obs_encoder_t *enc){return QJsonObject{{"pointer",QString::number(reinterpret_cast<quintptr>(enc),16)},{"name",enc?obs_encoder_get_name(enc):""},{"encoder_type",enc?int(obs_encoder_get_type(enc)):-1},{"encoded_frames_valid_for_video_only",enc&&obs_encoder_get_type(enc)==OBS_ENCODER_VIDEO},{"encoded_frames",int(enc?obs_encoder_get_encoded_frames(enc):0)},{"active",enc&&obs_encoder_active(enc)}};};
  QJsonObject info{{"output",names[i]},{"active",obs_output_active(output)},{"reconnecting",obs_output_reconnecting(output)},{"total_bytes",qint64(obs_output_get_total_bytes(output))},{"total_frames",int(obs_output_get_total_frames(output))},{"generation",int(packetTaps[i].generation.load())},{"new_socket_loop_enabled",obs_data_get_bool(settings,"new_socket_loop_enabled")},{"video",encoder(video)},{"audio",encoder(audio)}};
#ifdef STUDIO_CLOCK_PRIVATE_OBSERVER
  if(privateClockSampling && encoderPolls.size()%5==0 && obs_output_active(output))info["private_clock"]=observePrivateClock(output);
#endif
  outputs.append(info);
 }
 encoderPolls.append(QJsonObject{{"unix_ms",QDateTime::currentMSecsSinceEpoch()},{"monotonic_ns",qint64(os_gettime_ns())},{"outputs",outputs},{"qa_media",QJsonObject{{"exists",bool(media)},{"state",int(media?obs_source_media_get_state(media):OBS_MEDIA_STATE_NONE)},{"time_ms",qint64(media?obs_source_media_get_time(media):0)},{"width",int(media?obs_source_get_width(media):0)},{"height",int(media?obs_source_get_height(media):0)}}}});
}
void writePacketTaps()
{
 QJsonArray result;
 for(size_t n=0;n<2;++n){const auto &tap=packetTaps[n];QJsonArray events;
  const size_t count=tap.count.load(std::memory_order_acquire);
  for(size_t i=0;i<count;++i){const auto &e=tap.events[i];events.append(QJsonObject{{"monotonic_ns",qint64(e.monotonicNs)},{"unix_ms",qint64(e.unixMs)},{"generation",int(e.generation)},{"type",int(e.type)},{"track",int(e.track)},{"dts",qint64(e.dts)},{"pts",qint64(e.pts)},{"dts_usec",qint64(e.dtsUsec)},{"sys_dts_usec",qint64(e.sysDtsUsec)},{"has_packet_time",e.hasPacketTime},{"cts",qint64(e.cts)},{"fer",qint64(e.fer)},{"ferc",qint64(e.ferc)},{"pir",qint64(e.pir)},{"timebase_num",e.timebaseNum},{"timebase_den",e.timebaseDen},{"bytes",qint64(e.bytes)},{"keyframe",e.keyframe}});}
  result.append(QJsonObject{{"output",n?"portrait":"main"},{"events",events},{"overflow",qint64(tap.overflow.load())},{"reconnect_generation",int(tap.generation.load())}});
 }
 QSaveFile out(root+"/packet-taps.json");if(out.open(QIODevice::WriteOnly)){out.write(QJsonDocument(QJsonObject{{"taps",result},{"encoder_polls",encoderPolls},{"encoder_poll_overflow",qint64(encoderPollOverflow)},{"scope","Bounded post-interleave observer before encoded_packet; no payload bytes copied or packet mutation; generation increments at attachment/reconnect, counters do not reset"}}).toJson());out.commit();}
}
void editWhenReady(std::function<void(QDialog *)> fn, std::shared_ptr<QElapsedTimer> clock)
{
 later(100,[fn,clock]{
  auto *d=qobject_cast<QDialog *>(qApp->activeModalWidget());
  if(!d || d->objectName()!="OBSBasicSettings" || !d->isVisible()){
   check(clock->elapsed()<20000,"native Settings becomes visible outside its constructor");
   editWhenReady(fn,clock);return;
  }
  check(child<QListWidget>(d,"listWidget")->count()>0,"Settings category initialization completed");
  child<QListWidget>(d,"listWidget")->setCurrentRow(0);fn(d);
  child<QDialogButtonBox>(d,"buttonBox")->button(QDialogButtonBox::Apply)->click();
  child<QDialogButtonBox>(d,"buttonBox")->button(QDialogButtonBox::Cancel)->click();
 });
}
void edit(std::function<void(QDialog *)> fn)
{
 auto clock=std::make_shared<QElapsedTimer>();clock->start();editWhenReady(fn,clock);
 child<QAction>(window,"action_Settings")->trigger();
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
	writePacketTaps();
	QFile f(root + (restoring ? "/restore.json" : "/runtime.json"));
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(
				QJsonObject{
					{"passed", error.isEmpty()},
					{"error", error},
					{"checks", checks},
					{"files", files},
					{"plans", plans},
					{"save_results", saves},
					{"payload_violation", payloadViolation},
					{"stopped_zero_samples", stoppedZeroSamples},
					{"normal_stop_preserved_pending_save", normalStopSaveCovered},
					{"completed_save_groups",
					 QJsonArray{completedSaveGroups[0], completedSaveGroups[1],
						    completedSaveGroups[2], completedSaveGroups[3]}},
					{"scope",
					 "Actual native frontend streaming + real owned MultiStreamManager, dual recording, two independent replay buffers and save/library, Simple and Advanced output modes, native main primary and portrait secondary, pause, reverse start/stop order, network recovery; loopback RTMP only. Executed only when the matching Windows report and media analysis pass; WARP virtual device, local destinations, synthetic media. Other phases are disabled in this fixture."}})
				.toJson());
	}
}
void finishWhenIdle(QString error, std::function<void()> cleanup, std::shared_ptr<QElapsedTimer> clock = {})
{
	if(shuttingDown)return;
	if (!clock) {
		clock = std::make_shared<QElapsedTimer>();
		clock->start();
	}
	OBSOutputAutoRelease a = obs_frontend_get_streaming_output(), b = obs_frontend_get_recording_output();
	auto c = companion(), d = secondary(), e = mainReplay(), f = portraitReplay();
	bool idle = !obs_video_active() && !obs_frontend_streaming_active() && !obs_frontend_recording_active() &&
		    !obs_frontend_replay_buffer_active() && !manager()->IsActive();
	for (auto *output : {a.Get(), b.Get(), c.Get(), d.Get(), e.Get(), f.Get()}) {
		idle &= !output || !obs_output_active(output);
	}
    for(auto *output:{e.Get(),f.Get()}){
        if(!output)continue;calldata_t cd{};
        const bool status=proc_handler_call(obs_output_get_proc_handler(output),"get_replay_status",&cd);
        idle &= status && !calldata_bool(&cd,"capturing") && !calldata_bool(&cd,"saving") && !calldata_bool(&cd,"stopping") && calldata_int(&cd,"retained_bytes")==0;
        calldata_free(&cd);
    }
	if (!idle) {
		if(clock->elapsed()>=15000 && !cleanupDeadlineMissed){
		 cleanupDeadlineMissed=true;
		 report(error+"; cleanup deadline missed: preserving failure while observing normal stop");
		}
		if(clock->elapsed()>=120000){report(error+"; cleanup observation ended with outputs still active");return;}
		QTimer::singleShot(100, qApp, [error, cleanup, clock] { finishWhenIdle(error, cleanup, clock); });
		return;
	}
	cleanup();
}
void finish(QString error)
{
	if(shuttingDown)return;
	if (done) return;
 const int holdSeconds=qBound(0,qEnvironmentVariableIntValue("OBS_STUDIO_WINDOWS_OUTPUTS_HOLD_ON_FAILURE"),30);
 if(preflightReady && !error.isEmpty() && holdSeconds>0 && !diagnosticHoldApplied){
  diagnosticHoldApplied=true;done=true;if(payloadTimer)payloadTimer->stop();
  pollEncoders();report(error+"; FAILED acceptance, diagnostic observation before Stop");
  QFile::copy(root+"/runtime.json",root+"/failure-before-diagnostic-hold.json");
  QTimer::singleShot(holdSeconds*1000,qApp,[error]{if(shuttingDown)return;done=false;finish(error);});
  return;
 }
	privateClockSampling=false;
	if (payloadTimer) payloadTimer->stop();
	done = true;
	if (!preflightReady) {
		report(error);
		return;
	}
	if (auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
		/* Settings::reject() asks to save dirty fields. Use its native Cancel
		 * action so a fixture failure cannot hide behind a second modal. */
		auto *buttons = d->findChild<QDialogButtonBox *>("buttonBox");
		if (d->objectName() == "OBSBasicSettings" && buttons && buttons->button(QDialogButtonBox::Cancel)) {
			buttons->button(QDialogButtonBox::Cancel)->click();
		} else {
			d->reject();
		}
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
	finishWhenIdle(error, [error]() {
        if(shuttingDown)return;
        detachPacketTaps();writePacketTaps();
		for (auto *p : receivers) {
			if (p && p->state() != QProcess::NotRunning) {
				p->terminate();
				if (!p->waitForFinished(1500)) {
					p->kill();
					p->waitForFinished(1500);
				}
			}
		}
		restoreManagedSources();
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
            if(shuttingDown)return;
			if (duplicated) {
				if (!restoring && qEnvironmentVariableIsSet("OBS_STUDIO_WINDOWS_OUTPUTS_KEEP_PROFILE") &&
				    error.isEmpty()) {
					QFile state(root + "/restore-state.json");
					if (state.open(QIODevice::WriteOnly)) {
						state.write(
							QJsonDocument(QJsonObject{{"original_profile", originalProfile},
										  {"qa_profile", qaProfile}})
								.toJson());
					}
				} else {
					obs_frontend_delete_profile(qaProfile.toUtf8().constData());
				}
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
                if(shuttingDown)return;
				obs_frontend_save();
				report(error + (cleanupDeadlineMissed ? "; cleanup deadline missed before eventual quiescence" : ""));
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
	receivers[index]->setWorkingDirectory(root);
	receivers[index]->start(server, {c.fileName()});
	check(receivers[index]->waitForStarted(2000), "loopback receiver starts");
}
QJsonObject encoderPlan(obs_output_t *output, const QString &kind, const QString &canvas)
{
	check(output && obs_output_active(output), "output to inspect is actually active");
	auto *v = obs_output_get_video_encoder(output);
	check(v != nullptr, "output owns an effective video encoder");
	OBSDataAutoRelease settings = obs_encoder_get_settings(v);
	video_format effectiveFormat = obs_encoder_get_preferred_video_format(v);
	if (effectiveFormat == VIDEO_FORMAT_NONE && obs_encoder_parent_video(v)) {
		effectiveFormat = video_output_get_info(obs_encoder_parent_video(v))->format;
	}
	QJsonArray audio;
	for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
		auto *a = obs_output_get_audio_encoder(output, i);
		if (!a) {
			break;
		}
		OBSDataAutoRelease d = obs_encoder_get_settings(a);
		audio.append(QJsonObject{{"id", obs_encoder_get_id(a)},
					 {"name", obs_encoder_get_name(a)},
					 {"mixer", int(obs_encoder_get_mixer_index(a))},
					 {"settings", QJsonDocument::fromJson(obs_data_get_json(d)).object()}});
	}
	return QJsonObject{{"kind", kind},
			   {"canvas", canvas},
			   {"phase", phase},
			   {"output", obs_output_get_name(output)},
			   {"encoder_id", obs_encoder_get_id(v)},
			   {"encoder_name", obs_encoder_get_name(v)},
			   {"width", int(obs_encoder_get_width(v))},
			   {"height", int(obs_encoder_get_height(v))},
			   {"fps_divisor", int(obs_encoder_get_frame_rate_divisor(v))},
			   {"scale_type", int(obs_encoder_get_scale_type(v))},
			   {"preferred_format", int(obs_encoder_get_preferred_video_format(v))},
			   {"effective_format", int(effectiveFormat)},
			   {"settings", QJsonDocument::fromJson(obs_data_get_json(settings)).object()},
			   {"audio", audio}};
}
void inspectPayload()
{
	auto main = mainReplay(), portrait = portraitReplay();
	for (auto *o : {main.Get(), portrait.Get()}) {
		if (!o || !obs_output_active(o)) {
			continue;
		}
		calldata_t cd{};
		const bool exists = proc_handler_call(obs_output_get_proc_handler(o), "get_replay_status", &cd);
		const auto retained = calldata_int(&cd, "retained_bytes"), limit = calldata_int(&cd, "limit_bytes");
		const bool capturing = calldata_bool(&cd, "capturing"), saving = calldata_bool(&cd, "saving");
		const bool violation = !exists || retained < 0 || limit < 0 || retained > limit ||
				       (capturing && limit <= 0);
		const bool stoppedZero = !capturing && !saving && retained == 0 && limit == 0;
		QJsonObject sample{{"phase", phase},
				   {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
				   {"output", obs_output_get_name(o)},
				   {"obs_output_active", obs_output_active(o)},
				   {"exists", exists},
				   {"capturing", capturing},
				   {"stopping", calldata_bool(&cd, "stopping")},
				   {"saving", saving},
				   {"retained_bytes", retained},
				   {"snapshot_bytes", calldata_int(&cd, "snapshot_bytes")},
				   {"limit_bytes", limit}};
		if (violation && !payloadViolation) {
			QFile diag(root + "/first-payload-violation.json");
			if (diag.open(QIODevice::WriteOnly)) {
				diag.write(QJsonDocument(sample).toJson());
			}
		}
		if (stoppedZero && ++stoppedZeroSamples == 1) {
			QFile diag(root + "/stopped-zero-lifecycle.json");
			if (diag.open(QIODevice::WriteOnly)) {
				diag.write(QJsonDocument(sample).toJson());
			}
		}
		payloadViolation |= violation;
		calldata_free(&cd);
	}
}
void replayResult(void *data, calldata_t *cd)
{
	const QString canvas = QString::fromUtf8(static_cast<const char *>(data));
	const bool success = calldata_bool(cd, "success");
	const QString path = QString::fromUtf8(calldata_string(cd, "path"));
	const QString error = QString::fromUtf8(calldata_string(cd, "error"));
	const qint64 request = calldata_int(cd, "request_id");
	QMetaObject::invokeMethod(
		qApp,
		[canvas, success, path, error, request] {
			if (done) {
				return;
			}
			saves.append(QJsonObject{{"phase", phase},
						 {"canvas", canvas},
						 {"success", success},
						 {"path", path},
						 {"error", error},
						 {"request_id", request}});
			if (success) {
				auto b = canvas == "main" ? mainReplay() : portraitReplay();
				auto *v = obs_output_get_video_encoder(b);
				files.append(QJsonObject{{"path", path},
							 {"kind", "clip"},
							 {"canvas", canvas},
							 {"phase", phase},
							 {"width", int(obs_encoder_get_width(v))},
							 {"height", int(obs_encoder_get_height(v))}});
			}
		},
		Qt::QueuedConnection);
}
void connectReplays()
{
	for (char *canvas : {mainTag, portraitTag}) {
		auto o = QString::fromUtf8(canvas) == "main" ? mainReplay() : portraitReplay();
		check(o && obs_output_active(o), "each native replay instance exists and starts");
		signal_handler_disconnect(obs_output_get_signal_handler(o), "save_result", replayResult, canvas);
		signal_handler_connect(obs_output_get_signal_handler(o), "save_result", replayResult, canvas);
		const QByteArray suffix = QByteArray(canvas);
		auto *capture = child<QAction>(window, ("studioClipCapture_" + suffix).constData());
		auto *menu = qobject_cast<QMenu *>(capture->parent());
		check(menu && QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection),
		      "clip menu refreshes its current backend state");
		check(!child<QAction>(window, ("studioClipForceStop_" + suffix).constData())->isVisible(),
		      "force stop action is hidden for a normally capturing buffer");
		if (phase == 1) {
			OBSDataAutoRelease settings = obs_output_get_settings(o);
			const QByteArray fixed = QByteArray("collision-proof-") + canvas;
			obs_data_set_string(settings, "format", fixed.constData());
			obs_output_update(o, settings);
		}
	}
}
void allActive()
{
	check(obs_frontend_streaming_active() && obs_frontend_recording_active() && obs_frontend_replay_buffer_active(),
	      "native streaming recording and replay are simultaneously active");
	OBSOutputAutoRelease s = obs_frontend_get_streaming_output(), r = obs_frontend_get_recording_output();
	auto x = companion(), t = secondary(), bm = mainReplay(), bp = portraitReplay();
	check(obs_encoder_get_width(obs_output_get_video_encoder(r)) == 640u &&
		      obs_encoder_get_height(obs_output_get_video_encoder(r)) == 360u,
	      "recording uses actual native Main640x360 resolution");
	check(obs_encoder_get_width(obs_output_get_video_encoder(s)) == 640 &&
		      obs_encoder_get_height(obs_output_get_video_encoder(s)) == 360,
	      "Main stream uses640x360 alongside independent Portrait");
	check(t && obs_output_active(t) && x && obs_output_active(x) && bm && obs_output_active(bm) && bp &&
		      obs_output_active(bp),
	      "two live destinations two recordings and two replay buffers are all active");
	check(obs_output_get_total_bytes(s) > 10000 && obs_output_get_total_bytes(t) > 10000 &&
		      obs_output_get_total_bytes(r) > 10000 && obs_output_get_total_bytes(x) > 10000,
	      "both streams and both recordings deliver real bytes");
	check(obs_output_get_video_encoder(bm) != obs_output_get_video_encoder(bp) &&
		      obs_output_get_video_encoder(bm) != obs_output_get_video_encoder(r) &&
		      obs_output_get_video_encoder(bp) != obs_output_get_video_encoder(x),
	      "both clip video encoders are exclusive and cannot inherit recording pause");
	check(obs_output_get_audio_encoder(bm, 0) != obs_output_get_audio_encoder(r, 0) &&
		      obs_output_get_audio_encoder(bp, 0) != obs_output_get_audio_encoder(x, 0) &&
		      obs_output_get_audio_encoder(bm, 0) != obs_output_get_audio_encoder(bp, 0),
	      "both clip audio encoders are exclusive and cannot inherit recording pause");
	for (auto *o : {s.Get(), t.Get(), r.Get(), x.Get(), bm.Get(), bp.Get()}) {
		check(obs_output_get_audio_encoder(o, 0), "every output has its selected audio encoder");
	}
    for(auto *output:{s.Get(),t.Get(),r.Get(),x.Get(),bm.Get(),bp.Get()}){
        OBSDataAutoRelease settings=obs_encoder_get_settings(obs_output_get_video_encoder(output));
        check(QByteArray(obs_data_get_string(settings,"preset"))=="ultrafast","every actual Windows software encoder uses the explicit ultrafast preset");
    }
	for (const auto &entry : {encoderPlan(s, "stream", "main"), encoderPlan(t, "stream", "portrait"),
				  encoderPlan(r, "recording", "main"), encoderPlan(x, "recording", "portrait"),
				  encoderPlan(bm, "clip", "main"), encoderPlan(bp, "clip", "portrait")}) {
		plans.append(entry);
	}
	if (phase == 0 || phase == 3) {
		for (const auto &pair : {std::pair{bm.Get(), s.Get()}, std::pair{bp.Get(), t.Get()}}) {
			auto clip = encoderPlan(pair.first, "clip", "follow-check"),
			     live = encoderPlan(pair.second, "stream", "follow-check");
			check(clip["scale_type"] == live["scale_type"],
			      "Follow clip freezes the same GPU scale filter as its effective stream");
			check(clip["fps_divisor"] == live["fps_divisor"],
			      "Follow clip freezes the same FPS divisor as its effective stream");
			check(clip["effective_format"] == live["effective_format"],
			      "Follow clip keeps the same effective pixel format as its stream");
		}
	}
	for (const auto &entry : {encoderPlan(bm, "clip", "main"), encoderPlan(bp, "clip", "portrait")}) {
		const bool portrait = entry["canvas"].toString() == "portrait";
		const bool custom = phase == 1 || phase == 2;
		const int target = portrait ? (custom ? 2200 : 2800) : (custom ? 4500 : 6200);
		check(entry["settings"].toObject()["bitrate"].toInt() == target,
		      "effective clip bitrate follows its canvas stream or explicit custom preset");
		check(entry["settings"].toObject()["rate_control"].toString() == "CBR",
		      "clip uses stream CBR independently from recording quality");
		check(entry["width"].toInt() == (portrait ? 1080 : 640) &&
			      entry["height"].toInt() == (portrait ? 1920 : 360),
		      "effective clip resolution follows the selected independent canvas");
		check(!entry["audio"].toArray().isEmpty() && entry["audio"].toArray()[0].toObject()["mixer"].toInt() ==
								     (custom ? (portrait ? 2 : 1) : 0),
		      "effective clip audio uses its explicit custom mix or effective stream mix");
	}
	QJsonObject snapshot;
	auto *replay = window->findChild<QObject *>("studioReplayOutputs");
	check(replay && QMetaObject::invokeMethod(replay, "Snapshot", Qt::DirectConnection,
						  Q_RETURN_ARG(QJsonObject, snapshot)),
	      "replay manager exposes an effective per-canvas snapshot");
	plans.append(QJsonObject{{"kind", "manager-snapshot"}, {"phase", phase}, {"snapshot", snapshot}});
	inspectPayload();
	check(!payloadViolation, "per-instance retained ring plus snapshot payload stays within configured ceiling");
}
void verifySaveResults(int before, std::shared_ptr<QElapsedTimer> clock)
{
	if (saves.size() < before + 2) {
		check(clock->elapsed() < 10000, "both asynchronous clip saves complete before their deadline");
		later(100, [before, clock] { verifySaveResults(before, clock); });
		return;
	}

	check(saves.size() == before + 2, "Save Both reports a completion for each canvas");
	QString aggregatePair;
	for (int i = before; i < saves.size(); ++i) {
		const auto result = saves[i].toObject();
		check(result["success"].toBool() && QFileInfo(result["path"].toString()).size() > 10000,
		      "both clips write media while all other outputs remain active");
		auto *library = child<QListWidget>(window, "studioClipLibrary");
		bool found = false;
		for (int j = 0; j < library->count(); ++j) {
			if (library->item(j)->data(Qt::UserRole).toString() == result["path"].toString()) {
				found = true;
				check(library->item(j)->data(Qt::UserRole + 1).toString() ==
					      result["canvas"].toString(),
				      "library stores the actual canvas identity for each clip");
				check(!library->item(j)->data(Qt::UserRole + 2).toString().isEmpty(),
				      "library stores a non-empty save pair identity");
				if (phase != 1) {
					const QString pair = library->item(j)->data(Qt::UserRole + 2).toString();
					if (aggregatePair.isEmpty()) {
						aggregatePair = pair;
					}
					check(pair == aggregatePair,
					      "Save Both including the legacy hotkey gives both files the same pair identity");
				}
			}
		}
		check(found, "the native library registers the clip independently");
	}
	inspectPayload();
	check(!payloadViolation, "payload ceiling also covers retained save snapshots");
	++completedSaveGroups[phase];
	--pendingSaveAssertions;
}
void saveClip()
{
	if (pendingSaveAssertions) {
		later(100, saveClip);
		return;
	}
	const int before = saves.size();
	if (phase == 1) {
		for (const char *name : {"studioClipSave_main", "studioClipSave_portrait"}) {
			auto *action = child<QAction>(window, name);
			auto *menu = qobject_cast<QMenu *>(action->parent());
			check(menu && QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection),
			      "per-canvas clip menu updates immediately");
			check(action->isEnabled(), "per-canvas Save menu action is enabled for an active buffer");
			action->trigger();
		}
	} else if (!hotkeyExercised && phase == 0) {
		struct Key {
			obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
			int count = 0;
		} key;
		obs_enum_hotkeys(
			[](void *opaque, obs_hotkey_id id, obs_hotkey_t *hotkey) {
				auto &key = *static_cast<Key *>(opaque);
				if (!strcmp(obs_hotkey_get_name(hotkey), "ReplayBuffer.Save")) {
					key.id = id;
					key.count++;
				}
				return true;
			},
			&key);
		check(key.count == 1,
		      "one legacy Save Replay binding serves both buffers without duplicate registrations");
		check(QMetaObject::invokeMethod(window, "ProcessHotkey", Qt::DirectConnection,
						Q_ARG(obs_hotkey_id, key.id), Q_ARG(bool, true)),
		      "native routed legacy replay hotkey invokes Save Both");
		hotkeyExercised = true;
	} else {
		obs_frontend_replay_buffer_save();
	}
	++pendingSaveAssertions;
	auto clock = std::make_shared<QElapsedTimer>();
	clock->start();
	later(100, [before, clock] { verifySaveResults(before, clock); });
}
void collectRecordings()
{
	char *p = obs_frontend_get_last_recording();
	QString path = QString::fromUtf8(p);
	bfree(p);
	files.append(QJsonObject{{"path", path},
				 {"kind", "recording"},
				 {"phase", phase},
				 {"width", 640},
				 {"height", 360}});
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
		    obs_frontend_replay_buffer_active() || pendingSaveAssertions || obs_video_active() ||
		    (t && obs_output_active(t)) || (x && obs_output_active(x))) {
			check(n < 100, "all dual-replay outputs stop within deadline");
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
		if (++phase == 1) {
			finish();
		} else {
			phaseRun();
		}
	});
}
void stopAll()
{
	if (pendingSaveAssertions || completedSaveGroups[phase] < 2) {
		later(100, stopAll);
		return;
	}
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
void awaitFailedPortraitSave(int before, QString previous, std::shared_ptr<QElapsedTimer> clock)
{
	if (saves.size() <= before) {
		check(clock->elapsed() < 10000, "failed Portrait save returns an explicit result before deadline");
		later(100, [before, previous, clock] { awaitFailedPortraitSave(before, previous, clock); });
		return;
	}
	const auto result = saves[before].toObject();
	check(saves.size() == before + 1 && result["canvas"].toString() == "portrait" && !result["success"].toBool() &&
		      !result["error"].toString().isEmpty(),
	      "only the failed Portrait save reports an explicit error completion");
	allActive();
	QJsonObject state;
	auto *manager = window->findChild<QObject *>("studioReplayOutputs");
	check(manager && QMetaObject::invokeMethod(manager, "Snapshot", Qt::DirectConnection,
						   Q_RETURN_ARG(QJsonObject, state)),
	      "failed save state is visible through the native manager");
	const auto portraitState = state["buffers"].toObject()["portrait"].toObject();
	check(!portraitState["saving"].toBool() && !portraitState["lastError"].toString().isEmpty(),
	      "failed Portrait save clears saving and exposes its error to the interface");
	check(portraitState["lastPath"].toString() == previous,
	      "failed save preserves the previous successful clip path");
	auto bp = portraitReplay();
	calldata_t cd{};
	proc_handler_call(obs_output_get_proc_handler(bp), "get_last_replay", &cd);
	check(QString::fromUtf8(calldata_string(&cd, "path")) == previous,
	      "plugin legacy last replay still points to the successful file");
	calldata_free(&cd);
	OBSDataAutoRelease settings = obs_output_get_settings(bp);
	obs_data_set_string(settings, "directory", (root + QString("/phase-%1/local").arg(phase)).toUtf8().constData());
	obs_output_update(bp, settings);
	later(1200, saveClip);
	later(4000, stopAll);
}
void finishMainRestartWhenReady(std::shared_ptr<QElapsedTimer> clock)
{
	auto main = mainReplay(), portrait = portraitReplay();
	check(portrait && obs_output_active(portrait) && obs_frontend_recording_active() &&
		      obs_frontend_streaming_active(),
	      "Portrait replay recording and live remain active through Main restart");
	if (!main || !obs_output_active(main)) {
		check(clock->elapsed() < 15000, "Main replay restart completes before deadline");
		later(100, [clock] { finishMainRestartWhenReady(clock); });
		return;
	}
	signal_handler_disconnect(obs_output_get_signal_handler(main), "save_result", replayResult, mainTag);
	signal_handler_connect(obs_output_get_signal_handler(main), "save_result", replayResult, mainTag);
	check(true, "restarted Main buffer is actually capturing");
	allActive();
	// The restarted ring must acquire a real five-second window before save.
	later(5500, saveClip);
	later(8000, stopAll);
}
void finishMainStopWhenReady(std::shared_ptr<QElapsedTimer> clock)
{
	auto main = mainReplay(), portrait = portraitReplay();
	const bool healthy = portrait && obs_output_active(portrait) && obs_frontend_recording_active() &&
			     obs_frontend_streaming_active();
	const bool stopped = !main || !obs_output_active(main);
	QJsonObject snapshot;
	auto *manager = window->findChild<QObject *>("studioReplayOutputs");
	if (manager) {
		QMetaObject::invokeMethod(manager, "Snapshot", Qt::DirectConnection,
					  Q_RETURN_ARG(QJsonObject, snapshot));
	}
	QSaveFile diagnostic(root + "/main-stop-lifecycle.json");
	if (diagnostic.open(QIODevice::WriteOnly)) {
		diagnostic.write(
			QJsonDocument(QJsonObject{{"phase", phase},
						  {"elapsed_ms", clock->elapsed()},
						  {"main_stopped", stopped},
						  {"healthy_siblings", healthy},
						  {"snapshot", snapshot},
						  {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}})
				.toJson());
		diagnostic.commit();
	}
	check(healthy, "stopping Main replay keeps Portrait recording and live active during asynchronous shutdown");
	if (!stopped) {
		check(clock->elapsed() < 15000, "Main replay stops before its independent shutdown deadline");
		later(100, [clock] { finishMainStopWhenReady(clock); });
		return;
	}
	check(true, "stopping Main replay leaves Portrait replay recording and live active");
	auto *config = obs_frontend_get_profile_config();
	config_set_string(config, "StudioCanvas", "MainReplayPresetSource", "custom");
	config_set_string(config, "StudioCanvas", "MainReplayResolution", "99999x99999");
	bool failedStart = true;
	check(manager &&
		      QMetaObject::invokeMethod(manager, "StartTargets", Qt::DirectConnection,
						Q_RETURN_ARG(bool, failedStart), Q_ARG(QString, QString("main"))) &&
		      !failedStart,
	      "invalid Main replay resolution fails preflight without starting it");
	check(portrait && obs_output_active(portrait) && obs_frontend_recording_active() &&
		      obs_frontend_streaming_active(),
	      "one buffer preflight failure preserves its active sibling recording and multistream");
	config_set_string(config, "StudioCanvas", "MainReplayPresetSource", "stream");
	config_set_string(config, "StudioCanvas", "MainReplayResolution", "");
	auto *action = child<QAction>(window, "studioClipCapture_main");
	auto *menu = qobject_cast<QMenu *>(action->parent());
	check(menu && QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection) && !action->isChecked(),
	      "Main capture menu reflects the stopped backend immediately");
	action->trigger();
	auto restartClock = std::make_shared<QElapsedTimer>();
	restartClock->start();
	later(100, [restartClock] { finishMainRestartWhenReady(restartClock); });
}
void stopMainAfterSaveResults(std::shared_ptr<QElapsedTimer> clock)
{
	if (pendingSaveAssertions || completedSaveGroups[phase] < 1) {
		check(clock->elapsed() < 10000, "save results settle before the independent Main stop gesture");
		later(100, [clock] { stopMainAfterSaveResults(clock); });
		return;
	}
	auto *action = child<QAction>(window, "studioClipCapture_main");
	auto *menu = qobject_cast<QMenu *>(action->parent());
	check(menu && QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection) && action->isChecked(),
	      "Main capture menu reflects the live backend immediately");
	action->trigger();
	auto stopClock = std::make_shared<QElapsedTimer>();
	stopClock->start();
	later(100, [stopClock] { finishMainStopWhenReady(stopClock); });
}
void stopFilesKeepingStreams(std::shared_ptr<QElapsedTimer> clock, bool requested = false)
{
	if (!requested) {
		if (pendingSaveAssertions || completedSaveGroups[phase] < 2) {
			check(clock->elapsed() < 10000,
			      "both clip save results settle before stopping recording and replay");
			later(100, [clock] { stopFilesKeepingStreams(clock); });
			return;
		}
		auto mainClip = mainReplay(), portraitClip = portraitReplay();
		if (obs_frontend_replay_buffer_active() || obs_output_active(mainClip) ||
		    obs_output_active(portraitClip)) {
			check(clock->elapsed() < 15000, "normal replay stop finishes after both successful saves");
			check(obs_frontend_recording_active() && obs_frontend_streaming_active(),
			      "recording and streams remain active until normal replay stop completes");
			later(100, [clock] { stopFilesKeepingStreams(clock); });
			return;
		}
		normalStopSaveCovered = true;
		collectRecordings();
		OBSOutputAutoRelease stream = obs_frontend_get_streaming_output();
		bytesBefore[0] = obs_output_get_total_bytes(stream);
		obs_frontend_recording_stop();
		clock->restart();
		later(100, [clock] { stopFilesKeepingStreams(clock, true); });
		return;
	}
	OBSOutputAutoRelease stream = obs_frontend_get_streaming_output();
	auto portrait = secondary(), record = companion(), mainClip = mainReplay(), portraitClip = portraitReplay();
	check(obs_frontend_streaming_active() && stream && obs_output_active(stream) && portrait &&
		      obs_output_active(portrait),
	      "both live destinations remain active while recording and clip outputs stop asynchronously");
	bool filesIdle = !obs_frontend_recording_active() && !obs_frontend_replay_buffer_active();
	for (auto *output : {record.Get(), mainClip.Get(), portraitClip.Get()}) {
		filesIdle &= !output || !obs_output_active(output);
	}
	if (!filesIdle) {
		check(clock->elapsed() < 15000, "recording and replay stop independently before deadline");
		later(100, [clock] { stopFilesKeepingStreams(clock, true); });
		return;
	}
	check(obs_output_get_total_bytes(stream) > bytesBefore[0],
	      "stopping recording and both clip buffers leaves Main stream delivering new bytes");
	obs_frontend_streaming_stop();
	waitIdle();
}
void saveThenStopReplayNormally(std::shared_ptr<QElapsedTimer> clock)
{
	if (pendingSaveAssertions || completedSaveGroups[phase] < 1) {
		check(clock->elapsed() < 10000, "first save completes before Save Both followed by normal Stop");
		later(100, [clock] { saveThenStopReplayNormally(clock); });
		return;
	}
	saveClip();
	auto *replay = child<QObject>(window, "studioReplayOutputs");
	QJsonObject state;
	check(QMetaObject::invokeMethod(replay, "Snapshot", Qt::DirectConnection, Q_RETURN_ARG(QJsonObject, state)),
	      "observe both pending saves before immediate normal Stop");
	for (const QString &canvas : {QString("main"), QString("portrait")}) {
		check(state["buffers"].toObject()[canvas].toObject()["saving"].toBool(),
		      "Save Both has a pending request on each buffer before the Stop gesture");
	}
	check(QMetaObject::invokeMethod(replay, "StopTargets", Qt::DirectConnection, Q_ARG(QString, QString("both")),
					Q_ARG(bool, false)),
	      "normal Stop Both is requested immediately after Save Both");
	QJsonObject rejected;
	check(QMetaObject::invokeMethod(replay, "SaveTargets", Qt::DirectConnection,
					Q_RETURN_ARG(QJsonObject, rejected), Q_ARG(QString, QString("both"))),
	      "a new Save Both request can be checked while normal Stop is waiting");
	for (const QString &canvas : {QString("main"), QString("portrait")}) {
		check(!rejected[canvas].toObject()["accepted"].toBool(),
		      "stopping buffer rejects a new save without replacing the pending request");
	}
	clock->restart();
	later(100, [clock] { stopFilesKeepingStreams(clock); });
}
void failPortraitAfterSaveResults(std::shared_ptr<QElapsedTimer> readyClock)
{
	if (pendingSaveAssertions || completedSaveGroups[phase] < 1) {
		check(readyClock->elapsed() < 10000,
		      "successful saves settle before the intentionally unwritable save");
		later(100, [readyClock] { failPortraitAfterSaveResults(readyClock); });
		return;
	}
	QFile blocked(root + "/blocked-directory");
	check(blocked.open(QIODevice::WriteOnly), "failure fixture creates a regular file as an invalid directory");
	blocked.write("Only this clip save must fail.\n");
	blocked.close();
	auto bp = portraitReplay();
	OBSDataAutoRelease d = obs_output_get_settings(bp);
	obs_data_set_string(d, "directory", blocked.fileName().toUtf8().constData());
	obs_output_update(bp, d);
	QJsonObject state, result;
	auto *manager = window->findChild<QObject *>("studioReplayOutputs");
	check(manager && QMetaObject::invokeMethod(manager, "Snapshot", Qt::DirectConnection,
						   Q_RETURN_ARG(QJsonObject, state)),
	      "capture last successful Portrait clip before the forced save error");
	const QString previous = state["buffers"].toObject()["portrait"].toObject()["lastPath"].toString();
	const int before = saves.size();
	check(QMetaObject::invokeMethod(manager, "SaveTargets", Qt::DirectConnection, Q_RETURN_ARG(QJsonObject, result),
					Q_ARG(QString, QString("portrait"))) &&
		      result["portrait"].toObject()["accepted"].toBool(),
	      "Portrait manager accepts the intentionally unwritable save request");
	auto clock = std::make_shared<QElapsedTimer>();
	clock->start();
	later(100, [before, previous, clock] { awaitFailedPortraitSave(before, previous, clock); });
}
void challengeReady()
{
	connectReplays();
	allActive();
	if (phase == 0) {
		later(4500, [] { obs_frontend_recording_pause(true); });
		later(4900, [] {
			auto x = companion();
			check(obs_frontend_recording_paused() && obs_output_paused(x),
			      "Pause affects both recordings while streams and replay stay active");
			check(obs_frontend_streaming_active() && obs_frontend_replay_buffer_active(),
			      "recording pause leaves live and clip buffer running");
			saveClip();
		});
		later(6500, [] { obs_frontend_recording_pause(false); });
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
			for (const auto &entry : manager()->Snapshot()) {
				if (entry.id == "dual-replay-local-b") {
					live = entry.state == MultiStreamChannelState::Live &&
					       entry.health.reconnects >= 1;
				}
			}
			check(live, "vertical reconnects while recording and clips remain active");
			auto clock = std::make_shared<QElapsedTimer>();
			clock->start();
			saveThenStopReplayNormally(clock);
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
		later(7600, [] {
			auto clock = std::make_shared<QElapsedTimer>();
			clock->start();
			failPortraitAfterSaveResults(clock);
		});
	} else {
		later(5600, saveClip);
		later(7400, [] {
			auto clock = std::make_shared<QElapsedTimer>();
			clock->start();
			stopMainAfterSaveResults(clock);
		});
	}
}
void waitSixOutputs(int tries = 0)
{
	if (tries == 0) {
		startupClock.start();
	}
	OBSOutputAutoRelease stream = obs_frontend_get_streaming_output(), record = obs_frontend_get_recording_output();
	auto second = secondary(), portraitRecord = companion(), main = mainReplay(), portrait = portraitReplay();
	bool ready = obs_frontend_streaming_active() && obs_frontend_recording_active() &&
		     obs_frontend_replay_buffer_active();
	QJsonObject outputs;
	const std::pair<const char *, obs_output_t *> list[] = {
		{"stream_main", stream.Get()},    {"stream_portrait", second.Get()},
		{"recording_main", record.Get()}, {"recording_portrait", portraitRecord.Get()},
		{"clip_main", main.Get()},        {"clip_portrait", portrait.Get()}};
	for (const auto &entry : list) {
		auto *output = entry.second;
		const bool active = output && obs_output_active(output);
		ready &= active;
		QJsonObject info{{"exists", output != nullptr}, {"active", active}};
		if (output) {
			info["name"] = obs_output_get_name(output);
			info["last_error"] = obs_output_get_last_error(output);
			info["bytes"] = qint64(obs_output_get_total_bytes(output));
			if (auto *v = obs_output_get_video_encoder(output)) {
				info["encoder_id"] = obs_encoder_get_id(v);
				info["encoder_name"] = obs_encoder_get_name(v);
				info["width"] = int(obs_encoder_get_width(v));
				info["height"] = int(obs_encoder_get_height(v));
                OBSDataAutoRelease settings=obs_encoder_get_settings(v);
                info["effective_settings"]=QJsonDocument::fromJson(obs_data_get_json(settings)).object();
			}
		}
		outputs[entry.first] = info;
	}
	for (auto *output : {stream.Get(), second.Get(), record.Get(), portraitRecord.Get()}) {
		ready &= output && obs_output_get_total_bytes(output) > 10000;
	}
	QJsonObject snapshot;
	if (auto *manager = window->findChild<QObject *>("studioReplayOutputs")) {
		QMetaObject::invokeMethod(manager, "Snapshot", Qt::DirectConnection,
					  Q_RETURN_ARG(QJsonObject, snapshot));
	}
	auto *modal = qApp->activeModalWidget();
	QSaveFile diagnostic(root + QString("/startup-phase-%1.json").arg(phase));
	if (diagnostic.open(QIODevice::WriteOnly)) {
		diagnostic.write(
			QJsonDocument(QJsonObject{{"phase", phase},
						  {"ready", ready},
						  {"poll", tries},
						  {"elapsed_ms", startupClock.elapsed()},
						  {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
						  {"outputs", outputs},
						  {"replay", snapshot},
						  {"modal_class", modal ? modal->metaObject()->className() : ""},
						  {"modal_object", modal ? modal->objectName() : QString()},
						  {"modal_title", modal ? modal->windowTitle() : QString()}})
				.toJson());
		diagnostic.commit();
	}
	if (!ready) {
		check(startupClock.elapsed() < 15000,
		      "six concurrent outputs and their first media bytes become ready before deadline");
		later(100, [tries] { waitSixOutputs(tries + 1); });
		return;
	}
	check(true, "all six outputs are actually ready before relative challenge timers begin");
	challengeReady();
}
void recordThenWait()
{
	obs_frontend_recording_start();
	if (done) {
		return;
	}
	waitSixOutputs();
}
void exercise()
{
 privateClockSampling=true;
 packetTapTimer=new QTimer(qApp);packetTapTimer->setInterval(20);
 QObject::connect(packetTapTimer,&QTimer::timeout,qApp,attachPacketTaps);
 packetTapTimer->start();attachPacketTaps();
 encoderPollTimer=new QTimer(qApp);encoderPollTimer->setInterval(200);
 QObject::connect(encoderPollTimer,&QTimer::timeout,qApp,pollEncoders);encoderPollTimer->start();pollEncoders();
	if (phase == 0) {
		// Both Follow Stream plans are resolved before any network output starts.
		obs_frontend_replay_buffer_start();
		if (done) {
			return;
		}
		QJsonObject offline;
		auto *replay = window->findChild<QObject *>("studioReplayOutputs");
		check(replay && QMetaObject::invokeMethod(replay, "Snapshot", Qt::DirectConnection,
							  Q_RETURN_ARG(QJsonObject, offline)),
		      "Simple Follow presets expose their offline plans before streaming");
		plans.append(QJsonObject{{"kind", "offline-snapshot"}, {"phase", phase}, {"snapshot", offline}});
		for (const char *canvas : {"main", "portrait"}) {
			check(offline["buffers"].toObject()[canvas].toObject()["preset"].toObject()["origin"].toString() ==
				      "stream-offline",
			      "Simple Follow buffer resolves its own offline stream template");
		}
		goLive();
		if (done) {
			return;
		}
		later(1000, recordThenWait);
	} else if (phase == 1) {
		obs_frontend_replay_buffer_start();
		if (done) {
			return;
		}
		later(900, [] { obs_frontend_recording_start(); });
		later(1500, [] {
			goLive();
			if (!done) {
				waitSixOutputs();
			}
		});
	} else if (phase == 2) {
		obs_frontend_recording_start();
		if (done) {
			return;
		}
		obs_frontend_replay_buffer_start();
		if (done) {
			return;
		}
		later(1200, [] {
			goLive();
			if (!done) {
				waitSixOutputs();
			}
		});
	} else {
		obs_frontend_replay_buffer_start();
		if (done) {
			return;
		}
		later(1000, [] {
			goLive();
			if (done) {
				return;
			}
			recordThenWait();
		});
	}
}
void verifySavedSettings(bool stream)
{
	auto *c = obs_frontend_get_profile_config();
	for (const char *canvas : {"Main", "Portrait"}) {
		const QString prefix = QString::fromUtf8(canvas);
		const QByteArray sourceKey = (prefix + "ReplayPresetSource").toUtf8();
		const QString origin = QString::fromUtf8(config_get_string(c, "StudioCanvas", sourceKey.constData()));
		const QString normalized = origin.isEmpty() ? "stream" : origin;
		QFile diagnostic(root + QString("/preferences-phase-%1-%2.json").arg(phase).arg(prefix));
		if (diagnostic.open(QIODevice::WriteOnly)) {
			diagnostic.write(
				QJsonDocument(
					QJsonObject{{"raw_origin", origin},
						    {"effective_origin", normalized},
						    {"expected_origin", stream ? "stream" : "custom"},
						    {"has_user_value",
						     config_has_user_value(c, "StudioCanvas", sourceKey.constData())},
						    {"mix", qint64(config_get_int(
								    c, "StudioCanvas",
								    (prefix + "ReplayAudioMix").toUtf8().constData()))},
						    {"bitrate",
						     qint64(config_get_int(
							     c, "StudioCanvas",
							     (prefix + "ReplayBitrate").toUtf8().constData()))}})
					.toJson());
		}
		check(normalized == (stream ? "stream" : "custom"),
		      "profile persists each effective canvas preset origin including default follow");
		check(config_get_int(c, "StudioCanvas", (prefix + "ReplayAudioMix").toUtf8().constData()) ==
			      (prefix == "Main" ? 1 : 2),
		      "profile persists independent custom mixer identities");
		check(config_get_int(c, "StudioCanvas", (prefix + "ReplayBitrate").toUtf8().constData()) ==
			      (prefix == "Main" ? 4500 : 2200),
		      "profile persists each custom bitrate even when following stream");
	}
	edit([stream](QDialog *d) {
		for (const char *canvas : {"Main", "Portrait"}) {
			const QString prefix = QString("studioCanvas") + canvas;
			check(child<QComboBox>(d, (prefix + "ReplayPresetSource").toUtf8().constData())
					      ->currentData()
					      .toString() == (stream ? "stream" : "custom"),
			      "reopened Settings restores each preset origin");
			check(child<QComboBox>(d, (prefix + "ReplayAudioMix").toUtf8().constData())
					      ->currentData()
					      .toInt() == (QString(canvas) == "Main" ? 1 : 2),
			      "reopened Settings restores independent mixer selections");
		}
	});
}
void restoreRun()
{
	QFile f(root + "/restore-state.json");
	check(f.open(QIODevice::ReadOnly), "restart stage reads the retained QA profile identity");
	const auto state = QJsonDocument::fromJson(f.readAll()).object();
	check(state["original_profile"].toString() == originalProfile,
	      "restart begins in the preserved original profile");
	qaProfile = state["qa_profile"].toString();
	check(qaProfile.startsWith("Dual Replay QA "), "restart selects only its own reserved QA profile");
	duplicated = true;
	obs_frontend_set_current_profile(qaProfile.toUtf8().constData());
	later(1300, [] {
		char *p = obs_frontend_get_current_profile();
		const QString current = QString::fromUtf8(p);
		bfree(p);
		check(current == qaProfile, "retained temporary profile loads after process restart");
		verifySavedSettings(true);
		QFile runtime(root + "/runtime.json");
		check(runtime.open(QIODevice::ReadOnly), "restart reads only its own successful media report");
		const auto previous = QJsonDocument::fromJson(runtime.readAll()).object();
		check(previous["passed"].toBool(), "media stage succeeded before the restart stage");
		auto *library = child<QListWidget>(window, "studioClipLibrary");
		for (const auto &value : previous["files"].toArray()) {
			const auto file = value.toObject();
			if (file["kind"].toString() != "clip") {
				continue;
			}
			bool found = false;
			for (int i = 0; i < library->count(); ++i) {
				auto *item = library->item(i);
				if (item->data(Qt::UserRole).toString() != file["path"].toString()) {
					continue;
				}
				found = item->data(Qt::UserRole + 1).toString() == file["canvas"].toString() &&
					!item->data(Qt::UserRole + 2).toString().isEmpty();
			}
			check(found, "clip canvas and pair metadata survive process restart");
		}
		finish();
	});
}
void phaseRun()
{
	auto *c = obs_frontend_get_profile_config();
	const uint32_t mainWidth = 640, mainHeight = 360;
	config_set_uint(c, "Video", "BaseCX", mainWidth);
	config_set_uint(c, "Video", "BaseCY", mainHeight);
	config_set_uint(c, "Video", "OutputCX", mainWidth);
	config_set_uint(c, "Video", "OutputCY", mainHeight);
	config_set_uint(c, "Video", "FPSType", 1);
	config_set_uint(c, "Video", "FPSInt", 15);
	config_set_string(c, "Video", "FPSCommon", "15");
	config_set_string(c, "StudioCanvas", "MainStreamResolution", "640x360");
	obs_frontend_reset_video();
	obs_video_info vi{};
	check(obs_get_video_info(&vi) && vi.base_width == mainWidth && vi.base_height == mainHeight &&
		      vi.output_width == mainWidth && vi.output_height == mainHeight && vi.fps_num == 15 * vi.fps_den,
	      "temporary Windows profile uses Main640x360 and15fps conservatively");
	if (auto *item = obs_scene_find_source(fixture, obs_source_get_name(media))) {
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
		vec2 size{float(mainWidth), float(mainHeight)};
		obs_sceneitem_set_bounds(item, &size);
	}
	QString dir = root + QString("/phase-%1/local").arg(phase);
	QDir().mkpath(dir);
	config_set_int(c, "SimpleOutput", "VBitrate", 6200);
	config_set_int(c, "SimpleOutput", "ABitrate", 160);
	config_set_string(c, "SimpleOutput", "StreamAudioEncoder", "aac");
	config_set_string(c, "SimpleOutput", "RecAudioEncoder", "aac");
	config_set_int(c, "SimpleOutput", "RecTracks", 1);
	config_set_bool(c, "SimpleOutput", "VodTrackEnabled", false);
	config_set_string(c, "AdvOut", "AudioEncoder", "ffmpeg_aac");
	config_set_string(c, "AdvOut", "RecAudioEncoder", "ffmpeg_aac");
	for (int track = 1; track <= MAX_AUDIO_MIXES; ++track) {
		config_set_int(c, "AdvOut", QString("Track%1Bitrate").arg(track).toUtf8().constData(), 160);
	}
	config_set_int(c, "AdvOut", "TrackIndex", 1);
	config_set_bool(c, "AdvOut", "VodTrackEnabled", false);
	config_set_int(c, "AdvOut", "StreamMultiTrackAudioMixes", 1);
	config_set_bool(c, "AdvOut", "StreamMultiTrackAudio", false);
	config_set_bool(c, "Video", "AutoRemux", false);
	config_set_bool(c, "SimpleOutput", "UseAdvanced", true);
	config_set_string(c, "SimpleOutput", "Preset", "ultrafast");
	config_set_string(c, "SimpleOutput", "x264Settings", "keyint=30 threads=2");
	config_set_string(c, "SimpleOutput", "StreamEncoder",
			  obs_get_encoder_codec("obs_nvenc_h264_tex") ? "nvenc" : "x264");
	config_set_string(c, "SimpleOutput", "RecQuality", "HQ");
	config_set_string(c, "SimpleOutput", "RecEncoder", "x264_lowcpu");
	config_set_bool(c, "SimpleOutput", "RecRB", true);
	config_set_int(c, "SimpleOutput", "RecRBTime", 5);
	config_set_int(c, "SimpleOutput", "RecRBSize", 16);
	config_set_string(c, "SimpleOutput", "RecFormat2", "mkv");
	config_set_string(c, "AdvOut", "Encoder",
			  obs_get_encoder_codec("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "obs_x264");
	config_set_string(c, "AdvOut", "RecEncoder",
			  obs_get_encoder_codec("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "obs_x264");
	config_set_string(c, "AdvOut", "RecType", "Standard");
	config_set_string(c, "AdvOut", "RecFormat2", "mkv");
	config_set_string(c, "AdvOut", "RecFilePath", dir.toUtf8().constData());
	config_set_bool(c, "AdvOut", "RecRB", true);
	config_set_int(c, "AdvOut", "RecRBTime", 5);
	config_set_int(c, "AdvOut", "RecRBSize", 16);
	config_set_int(c, "AdvOut", "RecTracks", 1);
	config_set_bool(c, "AdvOut", "RecRescale", false);
	config_set_int(c, "AdvOut", "RecRescaleFilter", OBS_SCALE_DISABLE);
	config_set_bool(c, "Stream1", "EnableMultitrackVideo", false);
	config_set_bool(c, "Output", "DelayEnable", false);
	config_set_bool(c, "Output", "Reconnect", true);
	config_set_int(c, "Output", "RetryDelay", 1);
	config_set_int(c, "Output", "MaxRetries", 10);
	char *profilePath = obs_frontend_get_current_profile_path();
	QFile enc(QString::fromUtf8(profilePath) + "/streamEncoder.json");
	bfree(profilePath);
	check(enc.open(QIODevice::WriteOnly), "temporary profile encoder settings writable");
	enc.write(QJsonDocument(QJsonObject{{"rate_control", "CBR"},
					    {"bitrate", 6200},
					    {"keyint_sec", 2},
					    {"preset", obs_get_encoder_codec("obs_nvenc_h264_tex")
							       ? (phase == 3 ? "p1" : "p4")
							       : "ultrafast"},
					    {"multipass", phase == 3 ? "disabled" : "qres"},
					    {"lookahead", phase != 3}})
			  .toJson());
	enc.close();
	char *recordProfilePath = obs_frontend_get_current_profile_path();
	QFile recEnc(QString::fromUtf8(recordProfilePath) + "/recordEncoder.json");
	bfree(recordProfilePath);
	check(recEnc.open(QIODevice::WriteOnly), "temporary recording encoder settings writable");
	recEnc.write(
		QJsonDocument(
			QJsonObject{{"rate_control",
				     phase == 3 ? (obs_get_encoder_codec("obs_nvenc_h264_tex") ? "CQP" : "CRF") : "CBR"},
				    {"bitrate", 9000},
				    {"cqp", 20},
				    {"crf", 20},
				    {"keyint_sec", 1},
				    {"preset", obs_get_encoder_codec("obs_nvenc_h264_tex") ? (phase == 3 ? "p1" : "p4")
											   : "ultrafast"},
				    {"multipass", phase == 3 ? "disabled" : "qres"},
				    {"lookahead", phase != 3}})
			.toJson());
	recEnc.close();
	edit([dir](QDialog *d) {
		// Explicit native output gestures force a fresh Simple handler after its
        // pre-seeded low-CPU choice; the final selected mode remains Simple.
        child<QComboBox>(d,"outputMode")->setCurrentIndex(1);
        child<QComboBox>(d,"outputMode")->setCurrentIndex(0);
		child<QLineEdit>(d, "simpleOutputPath")->setText(dir);
		child<QPushButton>(d, "studioCanvasMulti")->click();
		child<QSpinBox>(d, "studioCanvasPortraitBitrate")->setValue(2800);
		child<QSpinBox>(d, "studioCanvasPortraitRecordingBitrate")->setValue(3600);
		choice(d, "studioCanvasRecordingCanvas", "both");
		choice(d, "studioCanvasReplayCanvas", "both");
		const bool custom = phase == 1 || phase == 2;
		choice(d, "studioCanvasMainReplayPresetSource", custom ? "custom" : "stream");
		choice(d, "studioCanvasPortraitReplayPresetSource", custom ? "custom" : "stream");
		for (const auto &entry : {std::pair{"studioCanvasMainReplayAudioMix", 1},
					  std::pair{"studioCanvasPortraitReplayAudioMix", 2}}) {
			auto *combo = child<QComboBox>(d, entry.first);
			int index = combo->findData(entry.second);
			check(index >= 0, "custom clip mixer choice exists");
			combo->setCurrentIndex(index);
		}

		child<QSpinBox>(d, "studioCanvasMainReplayBitrate")->setValue(4500);
		child<QSpinBox>(d, "studioCanvasPortraitReplayBitrate")->setValue(2200);
		choice(d, "studioCanvasMainReplayResolution", "");
		choice(d, "studioCanvasPortraitReplayResolution", "");
		for (const char *name :
		     {"studioCanvasMainReplayAudioBitrate", "studioCanvasPortraitReplayAudioBitrate"}) {
			child<QSpinBox>(d, name)->setValue(160);
		}
	});
	if (done) {
		return;
	}
	verifySavedSettings(!(phase == 1 || phase == 2));
	MultiStreamChannel a;
	a.id = "dual-replay-local-a";
	a.displayName = "Local primary";
	a.server = QString("rtmp://127.0.0.1:%1/live").arg(ports[0]).toStdString();
	a.streamKey = "test";
	a.videoLayout = MultiStreamVideoLayout::Main;
	MultiStreamChannel b = a;
	b.id = "dual-replay-local-b";
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
		obs_service_create("rtmp_common", "dual-replay-empty-before-promotion", nullptr, nullptr);
	obs_frontend_set_streaming_service(empty);
	listen(0);
	listen(1);
	later(900, exercise);
}
void checkPrivateConfiguration()
{
	const QString requested = qEnvironmentVariable("OBS_STUDIO_WINDOWS_OUTPUTS_CONFIG_ROOT");
	const QString expected = QFileInfo(requested).canonicalFilePath();
	check(!requested.isEmpty() && !expected.isEmpty() && QFileInfo(expected).isDir(),
	      "private configuration root is explicitly provided and exists");
	char *p = obs_frontend_get_current_profile_path();
	const QString profile = QFileInfo(QString::fromUtf8(p)).canonicalFilePath();
	bfree(p);
	check(!profile.isEmpty() && profile.startsWith(expected + "/"),
	      "effective profile path is inside the private configuration root before any mutation");
	auto *app = obs_frontend_get_app_config();
	QJsonObject locations;
	for (const char *key : {"Configuration", "SceneCollections", "Profiles", "PluginManagerSettings"}) {
        if (config_has_user_value(app,"Locations",key)) {
          const QString raw=QString::fromUtf8(config_get_string(app,"Locations",key));
          locations[key]=raw;
          check(!raw.isEmpty() && QFileInfo(raw).canonicalFilePath()==expected,
                "explicit portable Locations values stay inside own private root");
        } else {
          locations[key]="absent: portable default, effective profile guard passed";
        }
	}
	QFile diagnostic(root + (restoring ? "/restore-config-isolation.json" : "/config-isolation.json"));
	if (diagnostic.open(QIODevice::WriteOnly)) {
		diagnostic.write(QJsonDocument(QJsonObject{{"expected_root", expected},
							   {"effective_profile", profile},
							   {"locations", locations},
							   {"passed", true}})
					 .toJson());
	}
}
void run()
{
	window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	check(window && !obs_video_active(), "native OBS starts idle before combined test");
	checkPrivateConfiguration();
	originalChannels = manager()->ConfiguredChannels();
	payloadTimer = new QTimer(qApp);
	QObject::connect(payloadTimer, &QTimer::timeout, qApp, inspectPayload);
	payloadTimer->start(80);
	originalService = obs_frontend_get_streaming_service();
	original = obs_frontend_get_current_scene();
	studioMode = obs_frontend_preview_program_mode_active();
	char *p = obs_frontend_get_current_profile();
	originalProfile = QString::fromUtf8(p);
	bfree(p);
	preflightReady = true;
	if (restoring) {
		restoreRun();
		return;
	}
	preserveManagedSources();
	qaProfile = "Dual Replay QA " + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
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
		fixture = obs_scene_create("Dual Replay QA — local media");
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_bool(d, "is_local_file", true);
		obs_data_set_bool(d, "looping", true);
		obs_data_set_string(d, "local_file", (root + "/fixture.mp4").toUtf8().constData());
		media = obs_source_create("ffmpeg_source", "Dual Replay QA — pattern and tone", d, nullptr);
		obs_source_set_audio_mixers(media, 7);
		obs_scene_add(fixture, media);
		obs_frontend_set_current_scene(obs_scene_get_source(fixture));
		later(800, [] {
			portraitCanvas = vertical();
			OBSSourceAutoRelease p = obs_canvas_get_channel(portraitCanvas, 0);
			if (p && obs_source_get_type(p) == OBS_SOURCE_TYPE_TRANSITION) {
				OBSSourceAutoRelease target = obs_transition_get_active_source(p);
				portraitScene = obs_scene_from_source(target);
			} else {
				portraitScene = obs_scene_from_source(p);
			}
			struct FindPortrait {
				QString mainUuid;
				OBSScene scene;
			} wanted{QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(fixture))), {}};
			obs_canvas_enum_scenes(
				portraitCanvas,
				[](void *opaque, obs_source_t *source) {
					auto &wanted = *static_cast<FindPortrait *>(opaque);
					OBSDataAutoRelease data = obs_source_get_private_settings(source);
					if (!obs_source_removed(source) &&
					    QString::fromUtf8(obs_data_get_string(data, "otaku_main_scene_uuid")) ==
						    wanted.mainUuid) {
						wanted.scene = obs_scene_from_source(source);
						return false;
					}
					return true;
				},
				&wanted);
			if (wanted.scene) {
				portraitScene = wanted.scene;
			}
			check(bool(portraitScene), "persistent independent portrait scene ready");
			OBSDataAutoRelease d = obs_data_create();
			obs_data_set_int(d, "width", 160);
			obs_data_set_int(d, "height", 160);
			obs_data_set_int(d, "color", 0xff33bb55);
			OBSSourceAutoRelease color =
				obs_source_create_private("color_source_v3", "dual-replay-portrait-marker", d);
			marker = obs_scene_add(portraitScene, color);
			vec2 pos{40.f, 40.f};
			obs_sceneitem_set_pos(marker, &pos);
            if(qEnvironmentVariableIsSet("OBS_STUDIO_WINDOWS_OUTPUTS_CLEANUP_ONLY")){
                done=true;report("Intentional cleanup-only fixture interruption; no output started");
                QTimer::singleShot(500,qApp,[]{if(!shuttingDown && window)window->close();});return;
            }
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
void shutdownFixture()
{
 if(shuttingDown)return;
 shuttingDown=true;done=true;privateClockSampling=false;
 if(payloadTimer)payloadTimer->stop();
 detachPacketTaps();writePacketTaps();
 bool idle=!obs_video_active() && !obs_frontend_streaming_active() && !obs_frontend_recording_active() && !obs_frontend_replay_buffer_active();
 OBSOutputAutoRelease buffers[2]={mainReplay(),portraitReplay()};
 for(size_t i=0;i<2;++i){
  const auto &output=buffers[i];
  if(!output)continue;
  signal_handler_disconnect(obs_output_get_signal_handler(output),"save_result",replayResult,i==0?mainTag:portraitTag);
  calldata_t cd{};
  if(proc_handler_call(obs_output_get_proc_handler(output),"get_replay_status",&cd))
   idle &= !calldata_bool(&cd,"capturing") && !calldata_bool(&cd,"saving") && !calldata_bool(&cd,"stopping") && calldata_int(&cd,"retained_bytes")==0;
  else idle=false;
  calldata_free(&cd);
 }
 if(idle){
  for(auto *receiver:receivers){if(receiver && receiver->state()!=QProcess::NotRunning){receiver->terminate();if(!receiver->waitForFinished(1500)){receiver->kill();receiver->waitForFinished(1500);}}}
  if(marker && portraitScene){bool found=false;std::pair<obs_sceneitem_t *,bool *> match{marker,&found};obs_scene_enum_items(portraitScene,[](obs_scene_t *,obs_sceneitem_t *item,void *p){auto *pair=static_cast<std::pair<obs_sceneitem_t *,bool *> *>(p);if(item==pair->first)*pair->second=true;return true;},&match);if(found)obs_sceneitem_remove(marker);}
  marker=nullptr;managed.clear();
  if(fixture)obs_source_remove(obs_scene_get_source(fixture));
  if(media)obs_source_remove(media);
  fixture=nullptr;portraitScene=nullptr;media=nullptr;original=nullptr;portraitCanvas=nullptr;originalService=nullptr;
 }
 QSaveFile out(root+"/shutdown.json");if(out.open(QIODevice::WriteOnly)){out.write(QJsonDocument(QJsonObject{{"boundary","SCRIPTING_SHUTDOWN before ClearSceneData"},{"owned_outputs_quiescent",idle},{"owned_references_released",idle},{"runtime_result_unchanged",true},{"profile_or_UI_restored",false}}).toJson());out.commit();}
}
void event(obs_frontend_event e, void *)
{
 if(e==OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN){shutdownFixture();return;}
 if(shuttingDown)return;
 if(e==OBS_FRONTEND_EVENT_STREAMING_STARTING){attachPacketTaps();return;}
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		later(1600, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_WINDOWS_OUTPUTS_ROOT");
	restoring = qEnvironmentVariableIsSet("OBS_STUDIO_WINDOWS_OUTPUTS_RESTORE");
	server = qEnvironmentVariable("OBS_STUDIO_WINDOWS_OUTPUTS_SERVER");
	if (root.isEmpty() || server.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	later(180000, [] { finish("Dual Replay output test timed out"); });
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
