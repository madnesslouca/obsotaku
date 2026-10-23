/* Real libobs callback/PCM with synthetic audio and a private route adapter.
 * No app, GPU, encoder, physical device or authenticated service. */
#include <obs.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <array>
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
namespace {
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
 #ifdef _WIN32
 wchar_t modulePath[32768]{};
 GetModuleFileNameW(GetModuleHandleW(L"obs.dll"),modulePath,32768);
 obs_data_set_string(report,"core_path",std::filesystem::path(modulePath).u8string().c_str());
#else
 Dl_info lib{};dladdr(dlsym(RTLD_DEFAULT,"obs_startup"),&lib);obs_data_set_string(report,"core_path",lib.dli_fname?lib.dli_fname:"");
#endif
 bool started=obs_startup("en-US",nullptr,nullptr);check(started,"Real core startup");if(!started)return 3;
 obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_MONO;check(obs_reset_audio(&ai),"Real48k mono audio");studio_roots_geometry(true);
 obs_source_info info{};info.id="studio_audio_roots_tone";info.type=OBS_SOURCE_TYPE_INPUT;info.output_flags=OBS_SOURCE_AUDIO;info.get_name=name;info.create=create;info.destroy=destroy;obs_register_source(&info);
 auto *a=obs_source_create_private(info.id,"QA997",nullptr),*b=obs_source_create_private(info.id,"QA2003",nullptr);
 obs_source_set_audio_mixers(a,1);obs_source_set_audio_mixers(b,1);
 auto *ca=obs_canvas_create_private("QA root A",nullptr,PROGRAM),*cb=obs_canvas_create_private("QA root B",nullptr,PROGRAM);
 void *ra=studio_roots_attach(ca,true),*rb=studio_roots_attach(cb,true);
 auto *sa=obs_scene_create_private("QA scene A"),*sb=obs_scene_create_private("QA scene B");
 obs_scene_add(sa,a);obs_scene_add(sb,b);obs_source_set_volume(obs_scene_get_source(sa),.5f);
 obs_canvas_set_channel(ca,0,obs_scene_get_source(sa));
 audio_convert_info ac{};ac.samples_per_sec=48000;ac.format=AUDIO_FORMAT_FLOAT_PLANAR;ac.speakers=SPEAKERS_MONO;
 captured.reserve(48000*20);check(audio_output_connect(obs_get_audio(),0,&ac,capture,nullptr),"Connect real mixed PCM");
 std::thread producer(produce,a,b);
 tone("initial_single_scene",.02,0);
 auto *shadow=studio_roots_shadow(ra);
 tone("encoder_only_shadow_preserves_scene_gain",.02,0);
 studio_roots_detach(shadow);
 lockTest(studio_roots_view(ca),0);lockTest(studio_roots_view(ca),1);lockTest(sa,2);
 tone("audio_recovered_after_controlled_render_locks",.02,0);
 auto *inner=obs_scene_create_private("QA nested scene");
 auto *groupSource=obs_source_create_private("group","QA nested group",nullptr);
 auto *group=obs_group_from_source(groupSource);
 auto *direct=obs_scene_find_source(sa,"QA997");obs_sceneitem_remove(direct);
 auto *groupItem=obs_scene_add(group,a);obs_scene_add(inner,groupSource);obs_scene_add(sa,obs_scene_get_source(inner));
 tone("nested_scene_and_group_audio",.02,0);lockTest(group,3);
 obs_sceneitem_set_visible(groupItem,false);tone("hidden_group_child_silent",0,0);
 obs_sceneitem_set_visible(groupItem,true);tone("shown_group_child_restored",.02,0);

 obs_canvas_set_channel(cb,0,obs_scene_get_source(sb));tone("two_logical_views",.02,.03);
 obs_canvas_set_channel(cb,0,nullptr);tone("channel_removed",.02,0);
 obs_view_set_source(studio_roots_view(cb),0,obs_scene_get_source(sb));tone("view_set_source_route",.02,.03);
 obs_view_remove(studio_roots_view(cb));tone("view_removed_no_ghost_audio",.02,0);
 studio_roots_detach(rb);rb=nullptr;rb=studio_roots_attach(cb,false);tone("nonmixing_view_excluded_from_mix",.02,0);
 studio_roots_detach(rb);rb=nullptr;
 // Same input shared by two distinct scene roots: existing core deduplicates
 // the child into a single root, independent of scene gain.
 obs_sceneitem_t *oldItem=obs_scene_find_source(sb,"QA2003");if(oldItem)obs_sceneitem_remove(oldItem);obs_scene_add(sb,a);
 rb=studio_roots_attach(cb,true);tone("shared_source_two_views",.04,0);
 obs_view_remove(studio_roots_view(cb));tone("shared_source_view_removed",.02,0);
 studio_roots_detach(rb);rb=nullptr;
 // Registry routes must survive reset of obs_core_audio and its callback.
 stop=true;producer.join();audio_output_disconnect(obs_get_audio(),0,capture,nullptr);
 check(obs_reset_audio(&ai),"Audio reset while logical views survive");
 lastCapture=0;maxGap=0;stop=false;
 check(audio_output_connect(obs_get_audio(),0,&ac,capture,nullptr),"Reconnect mixed PCM after audio reset");
 producer=std::thread(produce,a,b);tone("logical_routes_survive_audio_reset",.02,0);
 obs_source_remove(a);tone("removed_source_silenced",0,0);
 // Exercise update/remove/reattach while source snapshots may be in flight.
 for(int i=0;i<80;i++) {
  auto *temporary=obs_canvas_create_private("QA transient root",nullptr,PROGRAM);
  obs_canvas_set_channel(temporary,0,obs_scene_get_source(sb));
  auto *route=studio_roots_attach(temporary,true);
  obs_view_remove(studio_roots_view(temporary));
  obs_canvas_release(temporary); // The audio callback must never retain raw view.
  studio_roots_detach(route);
 }
 check(true,"Repeated route attach/remove completes without deadlock");
 stop=true;producer.join();audio_output_disconnect(obs_get_audio(),0,capture,nullptr);
 check(producerLate.load()<150000000,"Synthetic producer stayed within150ms clock budget");
 obs_canvas_set_channel(ca,0,nullptr);obs_canvas_set_channel(cb,0,nullptr);studio_roots_detach(ra);if(rb)studio_roots_detach(rb);
 obs_scene_release(sa);obs_scene_release(sb);obs_scene_release(inner);obs_source_release(groupSource);obs_canvas_release(ca);obs_canvas_release(cb);obs_source_remove(b);obs_source_release(a);obs_source_release(b);
 obs_wait_for_destroy_queue();studio_roots_geometry(false);obs_shutdown();
 obs_data_set_bool(report,"passed",failures==0);obs_data_set_int(report,"failures",failures);obs_data_set_array(report,"checks",checks);obs_data_set_array(report,"measurements",measurements);
#ifdef _WIN32
 obs_data_set_string(report,"platform","Windows");
 obs_data_set_string(report,"scope","Native Windows core callback/PCM, synthetic sources, private headless route adapter and controlled render locks. No GPU, encoder, physical device or full OBS certification.");
#else
 obs_data_set_string(report,"platform","Linux");
 obs_data_set_string(report,"scope","Linux real core callback/PCM, synthetic sources, private headless route adapter, controlled render locks. No GPU, encoder, physical device, full OBS or Windows certification.");
#endif
 std::ofstream f(path);f<<obs_data_get_json(report)<<'\n';obs_data_array_release(checks);obs_data_array_release(measurements);obs_data_release(report);return failures?1:0;
}
