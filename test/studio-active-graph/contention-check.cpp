/* Real libobs callback/PCM with synthetic audio and a private route adapter.
 * No app, GPU, encoder, physical device or authenticated service. */
#include <obs.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <array>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
extern "C" void studio_roots_geometry(bool);
extern "C" obs_view_t *studio_roots_view(obs_canvas_t *);
extern "C" void *studio_roots_attach(obs_canvas_t *, bool);
extern "C" void studio_roots_detach(void *);
extern "C" void *studio_roots_shadow(void *);
extern "C" void studio_roots_lock(void *, int);
extern "C" void studio_roots_unlock(void *, int);
extern "C" long studio_graph_show(obs_source_t *);
extern "C" void hide_scene_audio_lock(obs_scene_t *);
extern "C" void hide_scene_audio_unlock(obs_scene_t *);
extern "C" size_t hide_item_pending(obs_sceneitem_t *, long *);
extern "C" int hide_group_video_probe(obs_scene_t *);
namespace {
struct CostNode {obs_source_t *self;std::vector<obs_source_t*> children;};
const char *costName(void*){return "Native independent layered DAG";}
void *costCreate(obs_data_t*,obs_source_t *s){return new CostNode{s};}
void costDestroy(void *p){delete static_cast<CostNode*>(p);}
void costEnum(void *p,obs_source_enum_proc_t cb,void *data){auto *n=static_cast<CostNode*>(p);for(auto *c:n->children)cb(n->self,c,data);}
struct CostDag {obs_source_t *root;std::vector<std::array<obs_source_t*,2>> layers;};
CostDag costDag(){obs_source_info i{};i.id="cost_native_dag";i.type=OBS_SOURCE_TYPE_INPUT;i.output_flags=OBS_SOURCE_AUDIO;i.get_name=costName;i.create=costCreate;i.destroy=costDestroy;i.enum_active_sources=costEnum;i.enum_all_sources=costEnum;obs_register_source(&i);CostDag d;d.root=obs_source_create_private(i.id,"Cost root",nullptr);for(int l=0;l<25;l++){d.layers.push_back({obs_source_create_private(i.id,"CostA",nullptr),obs_source_create_private(i.id,"CostB",nullptr)});if(l==0){for(auto *c:d.layers[l]){if(!obs_source_add_active_child(d.root,c))std::abort();static_cast<CostNode*>(obs_obj_get_data(d.root))->children.push_back(c);}}else for(auto *p:d.layers[l-1])for(auto *c:d.layers[l]){if(!obs_source_add_active_child(p,c))std::abort();static_cast<CostNode*>(obs_obj_get_data(p))->children.push_back(c);}}return d;}
void costDispose(CostDag &d){for(int l=23;l>=0;l--)for(auto *p:d.layers[l]){for(auto *c:d.layers[l+1])obs_source_remove_active_child(p,c);static_cast<CostNode*>(obs_obj_get_data(p))->children.clear();}for(auto *c:d.layers[0])obs_source_remove_active_child(d.root,c);static_cast<CostNode*>(obs_obj_get_data(d.root))->children.clear();for(auto &v:d.layers)for(auto *p:v)obs_source_release(p);obs_source_release(d.root);}
constexpr double Pi = 3.14159265358979323846;
std::mutex capturedMutex;
std::vector<float> captured;
std::atomic<bool> stop{false};
std::atomic<uint64_t> lastCapture{0}, maxGap{0}, captureCalls{0}, producerLate{0};
obs_data_array_t *checks = nullptr, *measurements = nullptr;
int failures = 0;
void check(bool passed, const char *name) {
 auto *e=obs_data_create();obs_data_set_string(e,"check",name);obs_data_set_bool(e,"passed",passed);
 obs_data_array_push_back(checks,e);obs_data_release(e);if(!passed)++failures;
 std::cout<<(passed?"PASS ":"FAIL ")<<name<<std::endl;
}
void maximum(std::atomic<uint64_t> &v,uint64_t n) {auto p=v.load();while(n>p&&!v.compare_exchange_weak(p,n)) {}}
void capture(void *,size_t mix,audio_data *data) {
 if(mix||!data->data[0])return;
 const uint64_t now=os_gettime_ns(),last=lastCapture.exchange(now);
 if(last)maximum(maxGap,now-last);++captureCalls;
 std::lock_guard lock(capturedMutex);auto *p=reinterpret_cast<float *>(data->data[0]);
 captured.insert(captured.end(),p,p+data->frames);
}
const char *name(void *){return "Root lifetime synthetic audio";}
void *create(obs_data_t *,obs_source_t *s){return s;}
void destroy(void *){}
void produce(obs_source_t *a,obs_source_t *b) {
 const uint64_t anchor=os_gettime_ns();uint64_t index=0;
 while(!stop.load()) {
  const uint64_t ts=anchor+index*1000000000ULL/48000;os_sleepto_ns_fast(ts);
  if(stop.load())break;auto now=os_gettime_ns();if(now>ts)maximum(producerLate,now-ts);
  std::array<float,960> x{},y{};
  for(size_t i=0;i<x.size();i++){double t=double(index+i)/48000.;x[i]=.04*std::sin(2*Pi*997*t);y[i]=.03*std::sin(2*Pi*2003*t);}
  obs_source_audio d{};d.frames=960;d.samples_per_sec=48000;d.speakers=SPEAKERS_MONO;d.format=AUDIO_FORMAT_FLOAT;d.timestamp=ts;
  d.data[0]=reinterpret_cast<uint8_t *>(x.data());obs_source_output_audio(a,&d);
  d.data[0]=reinterpret_cast<uint8_t *>(y.data());obs_source_output_audio(b,&d);index+=960;
 }
}
std::vector<float> sample(int settle=300,int duration=650) {
 std::this_thread::sleep_for(std::chrono::milliseconds(settle));
 {std::lock_guard lock(capturedMutex);captured.clear();}
 std::this_thread::sleep_for(std::chrono::milliseconds(duration));
 std::lock_guard lock(capturedMutex);return captured;
}
double amplitude(const std::vector<float> &s,double f) {
 if(s.empty())return 0;double x=0,y=0;
 for(size_t i=0;i<s.size();i++){double phase=2*Pi*f*i/48000.;x+=s[i]*std::cos(phase);y+=s[i]*std::sin(phase);}
 return 2*std::hypot(x,y)/s.size();
}
void tone(const char *stage,double expectedA,double expectedB) {
 auto s=sample();double a=amplitude(s,997),b=amplitude(s,2003);
 auto *e=obs_data_create();obs_data_set_string(e,"stage",stage);obs_data_set_int(e,"frames",s.size());
 obs_data_set_double(e,"amplitude997",a);obs_data_set_double(e,"amplitude2003",b);obs_data_array_push_back(measurements,e);obs_data_release(e);
 check(s.size()>24000, (std::string(stage)+": mixed PCM captured").c_str());
 auto close=[](double actual,double expected){return std::abs(actual-expected)<std::max(.0015,expected*.18);};
 check(close(a,expectedA),(std::string(stage)+": 997Hz amplitude").c_str());
 check(close(b,expectedB),(std::string(stage)+": 2003Hz amplitude").c_str());
}
void lockTest(void *view,int kind) {
 std::atomic<bool> held{false};
 std::thread blocker([&](){studio_roots_lock(view,kind);held=true;std::this_thread::sleep_for(std::chrono::milliseconds(400));studio_roots_unlock(view,kind);});
 while(!held.load())std::this_thread::yield();
 const auto before=captureCalls.load();maxGap=0;
 std::this_thread::sleep_for(std::chrono::milliseconds(300));const auto during=captureCalls.load()-before;
 blocker.join();std::this_thread::sleep_for(std::chrono::milliseconds(100));
 auto *e=obs_data_create();const char *stages[]={"mixes_lock","channels_lock","scene_video_lock","group_video_lock"};obs_data_set_string(e,"stage",stages[kind]);obs_data_set_int(e,"callbacks_while_held",during);obs_data_set_int(e,"max_gap_ns",maxGap.load());obs_data_array_push_back(measurements,e);obs_data_release(e);
 check(during>=8,(std::string(stages[kind])+": audio callbacks progress while held").c_str());
 check(maxGap.load()<150000000,(std::string(stages[kind])+": no audio callback gap above150ms").c_str());
}
}
int main(int argc,char **argv) {
 if(argc!=2)return 2;auto path=std::filesystem::absolute(argv[1]);if(std::filesystem::exists(path))return 2;
 checks=obs_data_array_create();measurements=obs_data_array_create();auto *report=obs_data_create();
 bool started=obs_startup("en-US",nullptr,nullptr);check(started,"Real core startup");if(!started)return 3;
 obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_MONO;check(obs_reset_audio(&ai),"Real48k mono audio");studio_roots_geometry(true);
 obs_source_info info{};info.id="studio_audio_hide_group_tone";info.type=OBS_SOURCE_TYPE_INPUT;info.output_flags=OBS_SOURCE_AUDIO;info.get_name=name;info.create=create;info.destroy=destroy;obs_register_source(&info);
 auto *a=obs_source_create_private(info.id,"QA997",nullptr),*b=obs_source_create_private(info.id,"QA2003",nullptr);
 obs_source_set_audio_mixers(a,1);obs_source_set_audio_mixers(b,1);
 auto *canvas=obs_canvas_create_private("QA hide group",nullptr,PROGRAM);void *route=studio_roots_attach(canvas,true);
 auto *scene=obs_scene_create_private("QA parent");auto *groupSource=obs_source_create_private("group","QA group",nullptr);
 auto *group=obs_group_from_source(groupSource);obs_scene_add(group,a);auto *groupItem=obs_scene_add(scene,groupSource);obs_scene_add(scene,b);
 obs_canvas_set_channel(canvas,0,obs_scene_get_source(scene));
 audio_convert_info ac{};ac.samples_per_sec=48000;ac.format=AUDIO_FORMAT_FLOAT_PLANAR;ac.speakers=SPEAKERS_MONO;
 captured.reserve(48000*10);check(audio_output_connect(obs_get_audio(),0,&ac,capture,nullptr),"Connect actual mixed PCM");
 auto unrelatedDag=costDag();std::thread producer(produce,a,b);tone("before_hold",.04,.03);lockTest(group,3);
 std::atomic<bool> held{false};std::atomic_uint64_t hideNs{0};
 const auto gateStart=os_gettime_ns();hide_scene_audio_lock(scene);const auto gateAcquired=os_gettime_ns();
 const auto gestureStart=os_gettime_ns();obs_sceneitem_set_visible(groupItem,false);hideNs=os_gettime_ns()-gestureStart;
 std::thread blocker([&]{studio_roots_lock(group,3);held=true;std::this_thread::sleep_for(std::chrono::milliseconds(400));studio_roots_unlock(group,3);});
 while(!held.load())std::this_thread::yield();
 long refs=0;const size_t pending=hide_item_pending(groupItem,&refs);const int videoProbe=hide_group_video_probe(group);const bool observedHeld=held.load();
 std::atomic<bool> costDone{false};std::atomic_uint64_t costElapsed{0};std::thread costWorker([&]{const auto start=os_gettime_ns();obs_source_inc_active(unrelatedDag.root);costElapsed=os_gettime_ns()-start;costDone=true;});
 while(studio_graph_show(unrelatedDag.root)==0&&!costDone.load())std::this_thread::yield();const bool independentUpdateObserved=!costDone.load();
 {std::lock_guard lock(capturedMutex);captured.clear();}
 const auto before=captureCalls.load();lastCapture=os_gettime_ns();maxGap=0;const auto gateNs=os_gettime_ns()-gateAcquired;hide_scene_audio_unlock(scene);
 const bool interleavingReached=pending>0&&refs>0&&videoProbe==EBUSY&&observedHeld&&gateNs<20000000;
 check(pending>0&&refs>0,"Visibility audio action queued and active_refs positive while parent audio gate held");
 check(videoProbe==EBUSY&&observedHeld,"Group video mutex actually held by blocker before parent audio release");
 check(gateNs<20000000,"Fixture gate released within20ms after queued visibility action and group video hold");
 std::this_thread::sleep_for(std::chrono::milliseconds(300));auto during=captureCalls.load()-before;
 const bool heldDuringMeasurement=hide_group_video_probe(group)==EBUSY;
 std::vector<float> duringPcm;{std::lock_guard lock(capturedMutex);duringPcm=captured;}
 const double during997=amplitude(duringPcm,997),during2003=amplitude(duringPcm,2003);
 blocker.join();costWorker.join();std::this_thread::sleep_for(std::chrono::milliseconds(150));
 auto*measurement=obs_data_create();obs_data_set_string(measurement,"stage","hide_group_with_independent_51_node_DAG_update");obs_data_set_bool(measurement,"independent_update_in_progress_before_audio_release",independentUpdateObserved);obs_data_set_int(measurement,"independent_update_ns",costElapsed.load());obs_data_set_int(measurement,"callbacks_during300ms",during);obs_data_set_int(measurement,"max_gap_ns",maxGap.load());obs_data_set_int(measurement,"gesture_ns",hideNs.load());obs_data_set_int(measurement,"fixture_gate_ns",gateNs);obs_data_set_int(measurement,"gate_acquire_wait_ns",gateAcquired-gateStart);obs_data_set_int(measurement,"pending_audio_actions",pending);obs_data_set_int(measurement,"active_refs",refs);obs_data_set_int(measurement,"group_video_trylock_result",videoProbe);obs_data_set_bool(measurement,"interleaving_reached",interleavingReached);obs_data_set_bool(measurement,"held_through300ms",heldDuringMeasurement);obs_data_set_int(measurement,"during_pcm_frames",duringPcm.size());obs_data_set_double(measurement,"during_amplitude997",during997);obs_data_set_double(measurement,"during_amplitude2003",during2003);obs_data_set_int(measurement,"producer_max_late_ns",producerLate.load());obs_data_array_push_back(measurements,measurement);obs_data_release(measurement);
 check(independentUpdateObserved,"Native independent DAG update observed in progress before audio release");
 check(heldDuringMeasurement,"Group video hold continues through300ms measurement interval");
 check(interleavingReached&&duringPcm.size()>=8192&&std::abs(during2003-.03)<.005,"Unrelated2003Hz audio remains in PCM during held group video mutex");
 check(interleavingReached&&duringPcm.size()>=8192&&during997<.006,"Hidden997Hz group is suppressed while unrelated PCM continues");
 check(interleavingReached&&during>=8,"Hiding group preserves unrelated-source mixed PCM heartbeat during held video mutex");
 check(interleavingReached&&maxGap.load()<150000000,"Hiding group has no global PCM gap above150ms");
 tone("hidden_group_unrelated_source_continues",0,.03);
 obs_sceneitem_set_visible(groupItem,true);tone("group_restored",.04,.03);
 check(producerLate.load()<150000000,"Synthetic source stays within150ms clock budget throughout hide control");
 stop=true;producer.join();audio_output_disconnect(obs_get_audio(),0,capture,nullptr);obs_source_dec_active(unrelatedDag.root);costDispose(unrelatedDag);
 obs_canvas_set_channel(canvas,0,nullptr);studio_roots_detach(route);obs_scene_release(scene);obs_source_release(groupSource);obs_canvas_release(canvas);obs_source_release(a);obs_source_release(b);obs_wait_for_destroy_queue();studio_roots_geometry(false);obs_shutdown();
 obs_data_set_bool(report,"passed",failures==0);obs_data_set_int(report,"failures",failures);obs_data_set_array(report,"checks",checks);obs_data_set_array(report,"measurements",measurements);
 obs_data_set_string(report,"scope","Graphics-free private core adapter, synthetic PCM. Hold400ms group.video_mutex then hide group with descendant997Hz; unrelated2003Hz must retain heartbeat. Private native graph candidate; independent51node/98edge DAG updates contend for gate. No installed app/device.");
 obs_data_save_json_safe(report,path.u8string().c_str(),"tmp",nullptr);obs_data_release(report);obs_data_array_release(checks);obs_data_array_release(measurements);return failures?1:0;
}
