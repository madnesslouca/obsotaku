#include "probe.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
struct Input {bool video; unsigned track; int64_t pts, dts, absolute; bool key;};
struct Result {std::string name; bool passed; std::string json;};
constexpr int64_t anchor = 10000000;
int rate(unsigned track){return track?30:15;}
std::vector<Input> inputs(unsigned videos,unsigned audios,int64_t ahead,int gopSeconds,int64_t videoPhase,bool reorder,bool priming)
{
 std::vector<Input> a;
 for(unsigned track=0;track<videos;++track)
  for(int64_t v=0;v<rate(track)*10;++v)
   {
    int64_t pts=v, phase=v%(rate(track)*gopSeconds);
    if(reorder&&phase>0&&phase<rate(track)*gopSeconds-1) pts+=phase%2?1:-1;
    a.push_back({true,track,pts,v,anchor+track*videoPhase+v*1000000/rate(track),v%(rate(track)*gopSeconds)==0});
   }
 for(unsigned track=0;track<audios;++track)
  for(int64_t i=priming?-1:0;i<470;++i)
   a.push_back({false,track,i*1024,i*1024,anchor+ahead+track*10000+i*1024*1000000/48000,true});
 std::stable_sort(a.begin(),a.end(),[](auto &x,auto &y){return x.absolute<y.absolute;});return a;
}
Result run(void *p,const std::string &name,unsigned audios,int64_t ahead,bool burst,
           unsigned videos=1,int gopSeconds=2,int64_t videoPhase=0,bool reorder=false,bool priming=false)
{
 auto input=inputs(videos,audios,ahead,gopSeconds,videoPhase,reorder,priming);
 auto feed=[&](const Input&i){studio_interleave_probe_feed_reordered(p,i.video,i.track,i.pts,i.dts,i.absolute,i.key);};
 if(burst)for(auto&i:input)if(i.video&&i.dts<rate(i.track)*3)feed(i);
 for(auto&i:input){if(burst&&i.video&&i.dts<rate(i.track)*3)continue;feed(i);}
 studio_interleave_state s{};studio_interleave_probe_state(p,&s);
 std::vector<studio_interleave_event> events(s.emitted);
 for(size_t i=0;i<events.size();++i)if(!studio_interleave_probe_event(p,i,&events[i]))std::abort();
 using Key=std::pair<int,int>;std::map<Key,studio_interleave_event> first,last;
 bool mono=true,payload=true;
 for(auto&e:events){Key k{e.type,e.track};if(last.count(k)&&e.dts_usec<last[k].dts_usec)mono=false;if(!first.count(k))first[k]=e;last[k]=e;if(e.type==1)payload&=(e.keyframe==(e.raw_index%(rate(e.track)*gopSeconds)==0));}
 bool idr=true,start=true,duration=true,contentStart=true,contentEnd=true;
 auto content=[&](const studio_interleave_event&e){return e.type==1?e.track*videoPhase+e.raw_index*1000000/rate(e.track):ahead+e.track*10000+e.raw_index*1000000/48000;};
 for(unsigned v=0;v<videos;++v){Key vk{1,int(v)};idr&=first.count(vk)&&first[vk].keyframe;
  for(unsigned a=0;a<audios;++a){Key ak{0,int(a)};
   bool present=first.count(vk)&&first.count(ak);start&=present;duration&=present;contentStart&=present;contentEnd&=present;if(!present)continue;
   start&=std::llabs(first[vk].dts_usec-first[ak].dts_usec)<=88000;
   duration&=std::llabs((last[vk].dts_usec-first[vk].dts_usec)-(last[ak].dts_usec-first[ak].dts_usec))<300000;
   contentStart&=std::llabs(content(first[vk])-content(first[ak]))<=88000;
   contentEnd&=std::llabs(content(last[vk])-content(last[ak]))<300000;
  }
 }
 std::vector<std::pair<std::string,bool>> checks={{"first_video_IDR",idr},{"DTS_monotonic",mono},{"keyframe_payload_consistent",payload},{"AV_start_delta_le_88ms",start},{"AV_duration_delta_lt_300ms",duration},{"absolute_content_start_delta_le_88ms",contentStart},{"absolute_content_end_delta_lt_300ms",contentEnd},{"fixture_peak_queue_le_512",s.peak_queued<=512},{"EPT_matches_remaining_video_by_PTS",s.timing_matches_queued_video}};
 studio_interleave_probe_reset(p);studio_interleave_state reset{};studio_interleave_probe_state(p,&reset);
 checks.push_back({"real_reset_clears_queue_offsets_and_flags",reset.emitted==0&&reset.queued==0&&reset.video_offset==0&&reset.audio_offsets[0]==0&&reset.audio_offsets[1]==0&&!reset.received_audio&&!reset.received_video&&reset.timings[0]==0&&reset.timings[1]==0&&reset.video_offsets[1]==0});
 bool pass=std::all_of(checks.begin(),checks.end(),[](auto&x){return x.second;});
 std::ostringstream out;out<<"{\"name\":\""<<name<<"\",\"passed\":"<<(pass?"true":"false")<<",\"checks\":{";
 for(size_t i=0;i<checks.size();++i){if(i)out<<",";out<<'"'<<checks[i].first<<"\":"<<(checks[i].second?"true":"false");}
 out<<"},\"emitted\":"<<s.emitted<<",\"queued\":"<<s.queued<<",\"peak_queued\":"<<s.peak_queued<<",\"tracks\":[";bool comma=false;
 for(auto &pair:first){if(comma)out<<",";comma=true;auto &e=pair.second;auto &end=last[pair.first];
  int64_t keyPTS=-1;for(auto &x:events)if(x.type==e.type&&x.track==e.track&&x.keyframe){keyPTS=x.pts;break;}
  out<<"{\"type\":"<<e.type<<",\"track\":"<<e.track<<",\"first_keyframe\":"<<(e.keyframe?"true":"false")<<",\"raw_first\":"<<e.raw_index<<",\"raw_last\":"<<end.raw_index<<",\"relative_first_pts\":"<<e.pts<<",\"first_keyframe_relative_pts\":"<<keyPTS<<",\"first_usec\":"<<e.dts_usec<<",\"last_usec\":"<<end.dts_usec<<",\"absolute_content_first_usec\":"<<content(e)<<",\"absolute_content_last_usec\":"<<content(end)<<",\"raw_offset\":"<<(e.type==1?s.video_offsets[e.track]:s.audio_offsets[e.track])<<"}";
 }
 out<<"]}";return {name,pass,out.str()};
}
Result noFutureIDR()
{
 void*p=studio_interleave_probe_create_multi(2,2);auto a=inputs(2,2,800000,20,0,false,false);
 auto feed=[&](const Input&i){studio_interleave_probe_feed_reordered(p,i.video,i.track,i.pts,i.dts,i.absolute,i.key);};
 for(auto&i:a)if(i.video&&i.dts<rate(i.track)*3)feed(i);
 for(auto&i:a){if(i.video&&i.dts<rate(i.track)*3)continue;feed(i);}
 studio_interleave_state st{};studio_interleave_probe_state(p,&st);
 bool noVideo=true;studio_interleave_event e{};
 for(size_t i=0;i<st.emitted;++i){studio_interleave_probe_event(p,i,&e);if(e.type==1)noVideo=false;}
 studio_interleave_probe_reset(p);studio_interleave_state reset{};studio_interleave_probe_state(p,&reset);
 bool clean=reset.queued==0&&reset.timings[0]==0&&reset.timings[1]==0&&!reset.received_video&&!reset.received_audio;
 std::vector<std::pair<std::string,bool>> checks={{"no_video_before_next_IDR",noVideo},{"finite_missing_IDR_queue_le_512",st.peak_queued<=512},{"EPT_matches_queued",st.timing_matches_queued_video},{"reset_cancels_wait_and_releases_packets",clean}};
 bool pass=std::all_of(checks.begin(),checks.end(),[](auto&x){return x.second;});std::ostringstream o;
 o<<"{\"name\":\"missing_future_IDR_stop_reset\",\"passed\":"<<(pass?"true":"false")<<",\"peak_queued\":"<<st.peak_queued<<",\"checks\":{";
 for(size_t i=0;i<checks.size();++i){if(i)o<<",";o<<'"'<<checks[i].first<<"\":"<<(checks[i].second?"true":"false");}o<<"}}";
 studio_interleave_probe_destroy(p);return {"missing_future_IDR_stop_reset",pass,o.str()};
}
int main(int argc,char **argv)
{
 if(argc!=2){std::cerr<<"Usage: check result.json\n";return 2;}
 const long allocationsBefore=studio_interleave_probe_allocations();std::vector<Result> results;
 auto fresh=[&](const char*name,unsigned audios,int64_t ahead,bool burst,unsigned videos=1,int gop=2,int64_t phase=0,bool reorder=false,bool priming=false){void*p=studio_interleave_probe_create_multi(videos,audios);if(!p)std::abort();results.push_back(run(p,name,audios,ahead,burst,videos,gop,phase,reorder,priming));studio_interleave_probe_destroy(p);};
 // The original seven cases and five criteria are retained; new criteria are additive.
 fresh("aligned_regular",1,0,false);fresh("aligned_video_burst",1,0,true);
 fresh("audio_ahead_800ms_regular",1,800000,false);fresh("audio_ahead_800ms_video_burst",1,800000,true);
 fresh("two_audio_tracks_ahead_video_burst",2,800000,true);
 void*p=studio_interleave_probe_create(1);results.push_back(run(p,"before_reset_aligned",1,0,false));studio_interleave_probe_reset(p);results.push_back(run(p,"after_reset_audio_ahead_burst",1,800000,true));studio_interleave_probe_destroy(p);
 fresh("audio_first_negative_800ms",1,-800000,false);
 fresh("negative_lead_video_burst",2,-800000,true);
 fresh("delayed_GOP_4s_burst",1,800000,true,1,4);
 fresh("two_videos_15_30fps_aligned",2,0,false,2);
 fresh("two_videos_15_30fps_ahead_burst",2,800000,true,2);
 fresh("two_videos_phase10ms_ahead_burst",2,800000,true,2,2,10000);
 fresh("two_videos_delayed_GOP4s_burst",2,800000,true,2,4);
 fresh("two_videos_reordered_PTS_aligned",2,0,false,2,2,0,true);
 fresh("two_videos_reordered_PTS_ahead_burst",2,800000,true,2,2,0,true);
 fresh("aligned_AAC_negative_priming",2,0,false,2,2,0,false,true);
 fresh("ahead_burst_AAC_negative_priming",2,800000,true,2,2,0,false,true);
 results.push_back(noFutureIDR());
 const long delayBefore=studio_interleave_probe_allocations();
 p=studio_interleave_probe_create(1);studio_interleave_probe_set_active_delay(p,true);
 results.push_back(run(p,"active_delay_normal",1,0,false));
 studio_interleave_probe_reset(p);
 results.push_back(run(p,"active_delay_after_reset_burst",1,800000,true));
 studio_interleave_probe_destroy(p);
 p=studio_interleave_probe_create_multi(2,2);studio_interleave_probe_set_active_delay(p,true);
 results.push_back(run(p,"active_delay_two_tracks_burst",2,800000,true,2));
 studio_interleave_probe_destroy(p);
 const long delayAfter=studio_interleave_probe_allocations();
 const bool delayOwnership=delayBefore==delayAfter;
 const long allocationsAfter=studio_interleave_probe_allocations();bool ownership=allocationsBefore==allocationsAfter;
 bool passed=ownership&&delayOwnership&&std::all_of(results.begin(),results.end(),[](auto&r){return r.passed;});
 std::ofstream out(argv[1]);out<<"{\"passed\":"<<(passed?"true":"false")<<",\"scope\":\"Real obs-output interleave/prune with controlled encoded packet metadata/payload; no codec DSP/GPU/network/Windows runtime. Private source only.\",\"ownership\":{\"passed\":"<<(ownership?"true":"false")<<",\"before\":"<<allocationsBefore<<",\"after\":"<<allocationsAfter<<"},\"active_delay_ownership\":{\"passed\":"<<(delayOwnership?"true":"false")<<",\"before\":"<<delayBefore<<",\"after\":"<<delayAfter<<"},\"cases\":[";
 for(size_t i=0;i<results.size();++i){if(i)out<<",";out<<results[i].json;std::cout<<results[i].name<<": "<<(results[i].passed?"PASS":"FAIL")<<"\n";}out<<"]}\n";return passed?0:1;
}
