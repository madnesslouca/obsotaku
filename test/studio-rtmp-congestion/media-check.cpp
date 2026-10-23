/* SPDX-License-Identifier: GPL-2.0-or-later
 * Opt-in private fixture. No account store, normal output handler or real device.
 * Native RTMP A and production manager B share one encoder with file/replay.
 * The host runner must provide a fresh private config and immutable libobs.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTimer>
#include <atomic>
#include <thread>
#include <cmath>
#include <vector>
#include <memory>
#include <stdexcept>
OBS_DECLARE_MODULE()
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{ error = "Congestion fixture cannot persist channel metadata"; return false; }
namespace {
QString root,server,python,proxyScript;
QElapsedTimer clock;
QJsonArray checks,samples,proxyEvents,phases,clips;
QProcess *receivers[2]{}, *proxy=nullptr;
int ports[2]{},proxyPort=0,stage=0;
qint64 phaseStart=0;
QTimer *timer=nullptr;
bool closing=false,done=false,limited=false,primaryDbr=false,ffmpegReceiver=false;
QString failure;
std::unique_ptr<MultiStreamManager> manager;
OBSCanvasAutoRelease canvas;
OBSSceneAutoRelease scene;
OBSSourceAutoRelease source;
OBSEncoderAutoRelease video,audio;
OBSOutputAutoRelease primary,recording,replay;
OBSServiceAutoRelease service;
std::atomic_int primaryStops{0};
std::atomic_uint64_t pcmFrames{0},pcmEnergy{0};
void pcmCapture(void*,size_t mix,audio_data*data){if(mix!=5||!data->data[0])return;auto*p=reinterpret_cast<float*>(data->data[0]);double sum=0;for(unsigned i=0;i<data->frames;++i)sum+=p[i]*p[i];pcmEnergy.fetch_add(uint64_t(sum*1000000000.));pcmFrames.fetch_add(data->frames);}
double pcmRms(){auto n=pcmFrames.load();return n?std::sqrt(double(pcmEnergy.load())/1e9/n):0;}
bool published(int i){QFile f(root+(i?"/b.log":"/a.log"));if(!f.open(QIODevice::ReadOnly))return false;auto log=f.readAll();return ffmpegReceiver?log.contains("Output #0, mp4"):log.contains("is publishing to path");}

struct Pattern {
 obs_source_t *source;
 std::atomic_bool stop{false};
 std::atomic_bool failed{false};
 std::atomic_uint64_t audioBlocks{0},videoFrames{0},maxLate{0};
 std::thread worker;
};
Pattern *pattern=nullptr;
const char *patternName(void*) { return "Private congestion clock"; }
void *patternCreate(obs_data_t *,obs_source_t *s)
{
 auto*p=new Pattern;p->source=s;
 p->worker=std::thread([p]{
  const uint64_t anchor=os_gettime_ns();uint64_t ai=0,vi=0;
  std::vector<uint8_t> pixels(640*360*4);
  while(!p->stop.load()){
   const uint64_t at=anchor+ai*1000000000ULL/48000;
   const uint64_t vt=anchor+vi*1000000000ULL/15;
   const uint64_t next=std::min(at,vt);os_sleepto_ns_fast(next);
   if(p->stop.load())break;
   const uint64_t now=os_gettime_ns(),late=now>next?now-next:0;
   if(late>p->maxLate.load())p->maxLate=late;
   if(late>200000000ULL){p->failed=true;break;}
   if(vt<=at){
    uint32_t seed=uint32_t(vi+1)*1664525u;
    for(unsigned y=0;y<360;++y)for(unsigned x=0;x<640;++x){
     seed=seed*1664525u+1013904223u;
     const auto i=(y*640+x)*4;
     pixels[i]=uint8_t(seed>>24);pixels[i+1]=uint8_t(seed>>16);pixels[i+2]=uint8_t(seed>>8);pixels[i+3]=255;
    }
    obs_source_frame frame{};frame.data[0]=pixels.data();frame.linesize[0]=640*4;
    frame.width=640;frame.height=360;frame.format=VIDEO_FORMAT_BGRA;frame.timestamp=vt;
    obs_source_output_video(p->source,&frame);++vi;++p->videoFrames;
   }else{
    float tone[960];for(unsigned i=0;i<960;++i)tone[i]=.06f*std::sin(2*3.141592653589793*997*double(ai+i)/48000.);
    obs_source_audio data{};data.data[0]=reinterpret_cast<uint8_t*>(tone);data.frames=960;data.samples_per_sec=48000;
    data.speakers=SPEAKERS_MONO;data.format=AUDIO_FORMAT_FLOAT;data.timestamp=at;
    obs_source_output_audio(p->source,&data);ai+=960;++p->audioBlocks;
   }
  }
 });
 pattern=p;return p;
}
void patternDestroy(void*data){auto*p=static_cast<Pattern*>(data);p->stop=true;if(p->worker.joinable())p->worker.join();if(pattern==p)pattern=nullptr;delete p;}
void check(bool pass,const char*name){checks.append(QJsonObject{{"name",name},{"passed",pass},{"at_ms",clock.elapsed()}});if(!pass)throw std::runtime_error(name);}
void command(const QJsonObject&value){proxy->write(QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n');}
OBSOutputAutoRelease outputB(){if(manager)for(auto&e:manager->ChannelOutputs())if(e.id=="congestion-b")return obs_output_get_ref(e.output);return {};}
QString identity(const void*p){return QString::number(reinterpret_cast<quintptr>(p),16);}
QJsonObject metrics(obs_output_t*o){return {{"active",o&&obs_output_active(o)},{"reconnecting",o&&obs_output_reconnecting(o)},
 {"bytes",qint64(o?obs_output_get_total_bytes(o):0)},{"frames",o?obs_output_get_total_frames(o):0},
 {"drops",o?obs_output_get_frames_dropped(o):0},{"congestion",o?double(obs_output_get_congestion(o)):0},
 {"video_encoder",o?identity(obs_output_get_video_encoder(o)):QString()}};}
void sample(){auto b=outputB();OBSDataAutoRelease settings=video?obs_encoder_get_settings(video):nullptr;
 QJsonObject value{{"at_ms",clock.elapsed()},{"stage",stage},{"a",metrics(primary)},{"b",metrics(b)},{"record",metrics(recording)},{"replay",metrics(replay)},
 {"configured_bitrate",qint64(settings?obs_data_get_int(settings,"bitrate"):0)},{"primary_stops",primaryStops.load()}};
 if(manager){auto snap=manager->Snapshot();if(!snap.empty()){value["b_dbr_requested"]=snap[0].dynamicBitrateRequested;value["b_dbr_effective"]=snap[0].dynamicBitrateEffective;value["b_dbr_reason"]=int(snap[0].dynamicBitrateReason);}}
 samples.append(value);command({{"snapshot",true}});
}
void phase(int value){stage=value;phaseStart=clock.elapsed();phases.append(QJsonObject{{"stage",stage},{"at_ms",phaseStart}});}
void stopOutputs(){if(manager)manager->Stop();if(primary&&obs_output_active(primary))obs_output_stop(primary);if(recording&&obs_output_active(recording))obs_output_stop(recording);if(replay&&obs_output_active(replay))obs_output_stop(replay);}
void stopProcess(QProcess*p){if(!p||p->state()==QProcess::NotRunning)return;p->terminate();if(!p->waitForFinished(3000)){p->kill();p->waitForFinished(3000);}}
void saved(void*,calldata_t*);
void finalize()
{
 if(done)return;done=true;timer->stop();
 const QJsonObject generator{{"audio_blocks",qint64(pattern?pattern->audioBlocks.load():0)},{"video_frames",qint64(pattern?pattern->videoFrames.load():0)},
  {"max_late_ns",qint64(pattern?pattern->maxLate.load():0)},{"failed",pattern&&pattern->failed.load()}};
 if(pattern){pattern->stop=true;if(pattern->worker.joinable())pattern->worker.join();}
 audio_output_disconnect(obs_get_audio(),5,pcmCapture,nullptr);
 if(replay)signal_handler_disconnect(obs_output_get_signal_handler(replay),"save_result",saved,nullptr);
 manager.reset();primary=nullptr;recording=nullptr;replay=nullptr;service=nullptr;video=nullptr;audio=nullptr;
 if(canvas)obs_canvas_set_channel(canvas,0,nullptr);source=nullptr;scene=nullptr;canvas=nullptr;
 if(proxy){command({{"stop",true}});proxy->closeWriteChannel();if(!proxy->waitForFinished(4000))stopProcess(proxy);}
 for(auto*p:receivers)stopProcess(p);
 QFile file(root+"/runtime.json");if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(QJsonObject{
 {"passed",failure.isEmpty()},{"error",failure},{"checks",checks},{"samples",samples},{"proxy",proxyEvents},{"phases",phases},{"clips",clips},
 {"receiver",ffmpegReceiver?"ffmpeg-listen-copy-single-file":"mediamtx-fmp4-recorder"},{"generator",generator},{"mixed_pcm_rms",pcmRms()},{"primary_dbr",primaryDbr},{"requested_bitrate",2500},{"requested_limit_bytes_per_second",5000},
 {"scope","Private Main640x36015 synthetic997Hz; shared encoder native RTMP A + manager B + file/replay; no user config/devices/accounts"}}).toJson());
 QTimer::singleShot(100,qApp,[]{qApp->quit();});
}
void finish(const QString&error={}){if(closing)return;closing=true;failure=error;phase(9);if(proxy)command({{"rate_bps",0}});stopOutputs();}
void saveClip(){calldata_t cd;calldata_init(&cd);proc_handler_call(obs_output_get_proc_handler(replay),"save_request",&cd);check(calldata_bool(&cd,"accepted"),"replay save accepted during concurrent outputs");calldata_free(&cd);}
void saved(void*,calldata_t*cd){const bool success=calldata_bool(cd,"success");const QString path=QString::fromUtf8(calldata_string(cd,"path"));
 QMetaObject::invokeMethod(qApp,[success,path]{clips.append(QJsonObject{{"success",success},{"path",path},{"at_ms",clock.elapsed()}});},Qt::QueuedConnection);}
void setup()
{
 obs_video_info vi{};check(obs_get_video_info(&vi)&&vi.fps_num==15&&vi.fps_den==1,"global cadence is exactly15fps");
 vi.base_width=vi.output_width=640;vi.base_height=vi.output_height=360;
 canvas=obs_canvas_create_private("congestion-private-canvas",&vi,ACTIVATE|MIX_AUDIO|SCENE_REF|EPHEMERAL);check(bool(canvas),"private canvas created");
 scene=obs_canvas_scene_create(canvas,"congestion-private-scene");source=obs_source_create_private("studio_congestion_clock","congestion-pattern",nullptr);
 check(bool(scene)&&bool(source),"sample-clock source created");obs_source_set_audio_mixers(source,1<<5);obs_scene_add(scene,source);obs_canvas_set_channel(canvas,0,obs_scene_get_source(scene));
 OBSDataAutoRelease enc=obs_data_create();obs_data_set_string(enc,"rate_control","CBR");obs_data_set_int(enc,"bitrate",2500);obs_data_set_string(enc,"preset","ultrafast");obs_data_set_int(enc,"keyint_sec",1);
 video=obs_video_encoder_create("obs_x264","congestion-shared-video",enc,nullptr);
 OBSDataAutoRelease audioSettings=obs_data_create();obs_data_set_int(audioSettings,"bitrate",128);
 audio=obs_audio_encoder_create("ffmpeg_aac","congestion-shared-audio",audioSettings,5,nullptr);check(bool(video)&&bool(audio),"one shared H264/AAC encoder pair");
 obs_encoder_set_video(video,obs_canvas_get_video(canvas));obs_encoder_set_audio(audio,obs_get_audio());
 OBSDataAutoRelease rs=obs_data_create();obs_data_set_string(rs,"path",(root+"/recording.mkv").toUtf8().constData());recording=obs_output_create("ffmpeg_muxer","congestion-record",rs,nullptr);
 OBSDataAutoRelease replaySettings=obs_data_create();
 obs_data_set_string(replaySettings,"directory",root.toUtf8().constData());obs_data_set_string(replaySettings,"format","clip-%CCYY-%MM-%DD-%hh-%mm-%ss");obs_data_set_string(replaySettings,"extension","mkv");obs_data_set_int(replaySettings,"max_time_sec",5);obs_data_set_int(replaySettings,"max_size_mb",64);obs_data_set_bool(replaySettings,"studio_disable_save_hotkey",true);
 replay=obs_output_create("replay_buffer","congestion-replay",replaySettings,nullptr);check(bool(recording)&&bool(replay),"file and replay consumers created");
 for(auto*o:{recording.Get(),replay.Get()}){obs_output_set_video_encoder(o,video);obs_output_set_audio_encoder(o,audio,0);}
 signal_handler_connect(obs_output_get_signal_handler(replay),"save_result",saved,nullptr);
 OBSDataAutoRelease ss=obs_data_create();obs_data_set_string(ss,"server",QString("rtmp://127.0.0.1:%1/live").arg(ports[0]).toUtf8().constData());obs_data_set_string(ss,"key","test");
 service=obs_service_create("rtmp_custom","congestion-primary-service",ss,nullptr);check(bool(service),"loopback primary service created");
 OBSDataAutoRelease primarySettings=obs_data_create();obs_data_set_bool(primarySettings,"dyn_bitrate",primaryDbr);obs_data_set_string(primarySettings,"bind_ip","127.0.0.1");primary=obs_output_create("rtmp_output","congestion-primary-a",primarySettings,nullptr);check(bool(primary),"native primary RTMP A created");
 obs_output_set_service(primary,service);obs_output_set_video_encoder(primary,video);obs_output_set_audio_encoder(primary,audio,0);
 signal_handler_connect(obs_output_get_signal_handler(primary),"stop",[](void*,calldata_t*){++primaryStops;},nullptr);
 manager=std::make_unique<MultiStreamManager>();MultiStreamChannel b;b.id="congestion-b";b.displayName="Congested local B";b.server=QString("rtmp://127.0.0.1:%1/live").arg(proxyPort).toStdString();b.streamKey="test";b.audioMixIndex=5;
 std::string error;check(manager->Configure({b},error),"production manager B configured");
 audio_convert_info convert{};convert.samples_per_sec=48000;convert.format=AUDIO_FORMAT_FLOAT_PLANAR;convert.speakers=SPEAKERS_STEREO;
 check(audio_output_connect(obs_get_audio(),5,&convert,pcmCapture,nullptr),"preflight listens to actual mixed PCM track6");
 phase(6);
}
void startOutputs(){
 std::string error;
 check(obs_output_start(recording)&&obs_output_start(replay)&&obs_output_start(primary),"record replay primary start accepted");
 MultiStreamOutputSettings options;options.dynamicBitrate=true;options.bindIp="127.0.0.1";options.reconnect={3,1};check(manager->Start(video,audio,options,error),"manager B starts with preserved DBR request");
 phase(1);
}
void tick()
{
 try{
  const auto elapsed=clock.elapsed()-phaseStart;
  if(stage==9){if((!primary||!obs_output_active(primary))&&(!recording||!obs_output_active(recording))&&(!replay||!obs_output_active(replay))&&(!manager||!manager->IsActive()))finalize();else if(elapsed>12000){failure+=" output stop exceeded12s";if(primary)obs_output_force_stop(primary);if(recording)obs_output_force_stop(recording);if(replay)obs_output_force_stop(replay);if(manager)manager->Stop(true);}return;}
  if(pattern&&pattern->failed)throw std::runtime_error("fixture sample-clock backlog exceeded200ms");
  if(clock.elapsed()>80000)throw std::runtime_error("fixture80s deadline");
  if(stage==0){if(proxyPort&&elapsed>1200)setup();return;}
  if(stage==6){if(elapsed<1500)return;check(pcmFrames>=48000&&pcmRms()>.01,"mixed PCM track6 has audible synthetic source before starting outputs");startOutputs();return;}
  sample();auto b=outputB();
  if(stage==1){if(elapsed>15000)throw std::runtime_error("both RTMP outputs never ready");if(primary&&b&&obs_output_get_total_bytes(primary)>50000&&obs_output_get_total_bytes(b)>50000&&published(0)&&published(1)){check(obs_output_active(recording)&&obs_output_active(replay),"all four consumers ready before measured phases");phase(2);}return;}
  check(primaryStops==0&&obs_output_active(primary)&&obs_output_active(recording)&&obs_output_active(replay),"healthy A file and replay remain active");
  check(obs_output_get_video_encoder(primary)==video&&obs_output_get_video_encoder(b)==video&&obs_output_get_video_encoder(recording)==video&&obs_output_get_video_encoder(replay)==video,"all consumers retain same video identity");
  auto snapshots=manager->Snapshot();OBSDataAutoRelease bs=obs_output_get_settings(b);
  check(snapshots.size()==1&&snapshots[0].dynamicBitrateRequested&&!snapshots[0].dynamicBitrateEffective&&snapshots[0].dynamicBitrateReason==MultiStreamDynamicBitrateReason::SharedEncoderAuthority&&!obs_data_get_bool(bs,"dyn_bitrate"),"B preserves DBR intent but cannot control shared encoder");
  check(obs_output_get_audio_encoder(primary,0)==audio&&obs_output_get_audio_encoder(b,0)==audio&&obs_output_get_audio_encoder(recording,0)==audio&&obs_output_get_audio_encoder(replay,0)==audio,"all consumers retain same audio identity");
  if(!primaryDbr){OBSDataAutoRelease vs=obs_encoder_get_settings(video);check(obs_data_get_int(vs,"bitrate")==2500,"fixed primary preserves configured encoder bitrate");}
  if(stage==2&&elapsed>=5000){command({{"rate_bps",5000}});phase(3);}
  else if(stage==3&&elapsed>=18000){saveClip();command({{"rate_bps",0}});phase(4);}
  else if(stage==4&&elapsed>=12000){saveClip();phase(5);}
  else if(stage==5&&elapsed>=2500){check(clips.size()==2&&clips[0].toObject()["success"].toBool()&&clips[1].toObject()["success"].toBool(),"both replay snapshots saved");finish();}
 }catch(const std::exception&e){finish(QString::fromUtf8(e.what()));}
}
int freePort(){QTcpServer server;check(server.listen(QHostAddress::LocalHost),"loopback port reserved");return server.serverPort();}
void run()
{
 clock.start();phase(0);
 try{
  check(!obs_frontend_streaming_active()&&!obs_frontend_recording_active()&&!obs_frontend_replay_buffer_active(),"frontend outputs idle in private instance");
  for(int i=0;i<2;++i){ports[i]=freePort();receivers[i]=new QProcess(qApp);QString name=i?"b":"a";QFile file(root+"/"+name+".yml");check(file.open(QIODevice::WriteOnly),"receiver config writable");
   file.write(QString("logLevel: info\nreadTimeout: 60s\nwriteTimeout: 60s\nrtsp: false\nrtmp: true\nrtmpAddress: 127.0.0.1:%1\nhls: false\nwebrtc: false\nsrt: false\nmoq: false\napi: false\nmetrics: false\npprof: false\nplayback: false\npathDefaults:\n  record: true\n  recordPath: '%2/%3/%%path/%%Y-%%m-%%d_%%H-%%M-%%S-%%f'\n  recordFormat: fmp4\n  recordPartDuration: 100ms\n  recordSegmentDuration: 1h\n  recordDeleteAfter: 0s\npaths:\n  all_others: {}\n").arg(ports[i]).arg(root).arg(name).replace("%%","%").toUtf8());file.close();
   receivers[i]->setProcessChannelMode(QProcess::MergedChannels);receivers[i]->setStandardOutputFile(root+"/"+name+".log");
   if(ffmpegReceiver){QDir().mkpath(root+"/"+name);receivers[i]->start(server,{"-hide_banner","-nostdin","-loglevel","info","-listen","1","-timeout","60","-analyzeduration","1000000","-probesize","500000","-i",QString("rtmp://127.0.0.1:%1/live/test").arg(ports[i]),"-map","0:v:0","-map","0:a:0","-c","copy","-movflags","frag_keyframe+empty_moov+default_base_moof",root+"/"+name+"/received.mp4"});}
   else receivers[i]->start(server,{file.fileName()});}
  proxy=new QProcess(qApp);proxy->setStandardErrorFile(root+"/proxy.stderr");
  QObject::connect(proxy,&QProcess::readyReadStandardOutput,qApp,[]{while(proxy->canReadLine()){auto object=QJsonDocument::fromJson(proxy->readLine()).object();object["observed_ms"]=clock.elapsed();proxyEvents.append(object);if(object["event"]=="ready")proxyPort=object["port"].toInt();}});
  proxy->start(python,{proxyScript,"--target-port",QString::number(ports[1])});
  timer=new QTimer(qApp);QObject::connect(timer,&QTimer::timeout,qApp,tick);timer->start(250);
 }catch(const std::exception&e){finish(QString::fromUtf8(e.what()));}
}
void event(obs_frontend_event e,void*){if(e==OBS_FRONTEND_EVENT_FINISHED_LOADING)QTimer::singleShot(1200,qApp,run);}
}
bool obs_module_load(){root=qEnvironmentVariable("OTAKUDOBS_CONGESTION_ROOT");server=qEnvironmentVariable("OTAKUDOBS_CONGESTION_SERVER");python=qEnvironmentVariable("OTAKUDOBS_CONGESTION_PYTHON");proxyScript=qEnvironmentVariable("OTAKUDOBS_CONGESTION_PROXY");ffmpegReceiver=qEnvironmentVariable("OTAKUDOBS_CONGESTION_RECEIVER")=="ffmpeg";primaryDbr=qEnvironmentVariableIntValue("OTAKUDOBS_CONGESTION_PRIMARY_DBR")!=0;
 if(root.isEmpty()||QFile::exists(root+"/runtime.json")||!QDir().mkpath(root))return false;
 obs_source_info info{};info.id="studio_congestion_clock";info.type=OBS_SOURCE_TYPE_INPUT;info.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;info.get_name=patternName;info.create=patternCreate;info.destroy=patternDestroy;obs_register_source(&info);obs_frontend_add_event_callback(event,nullptr);return true;}
void obs_module_unload(){obs_frontend_remove_event_callback(event,nullptr);if(timer)timer->stop();if(pattern){pattern->stop=true;if(pattern->worker.joinable())pattern->worker.join();}stopOutputs();for(auto*p:receivers)stopProcess(p);stopProcess(proxy);}
