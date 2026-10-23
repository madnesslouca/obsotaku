/* Private deterministic snapshot/retirement fixture; no stale pointer is dereferenced. */
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern struct obs_source_info slideshow_info,slideshow_info_mk2;
extern obs_source_t *studio_snapshot_get(obs_source_t *);
extern void studio_graph_tick(obs_source_t *);
extern void studio_graph_drain(void);
static FILE *report;static unsigned checks,failures;
static volatile long created,destroyed,transition_time_calls;
static volatile long reader_armed,writer_armed,avoided_stale;
static os_event_t *reader_gate,*release_gate,*writer_gate;
static obs_source_t *target_raw;
static obs_weak_source_t *target_weak;
const char *obs_module_text(const char*t){return t;}
bool obs_module_get_string(const char*t,const char**v){*v=t;return true;}
obs_module_t *obs_current_module(void){return NULL;}
uint64_t image_source_get_memory_usage(void*p){(void)p;abort();}
void image_source_preload_image(void*p){(void)p;abort();}
static void check(const char*n,bool ok){++checks;if(!ok)++failures;fprintf(report,"%s\t%s\n",ok?"PASS":"FAIL",n);fflush(report);}
static const char *name(void*p){(void)p;return "snapshot-lifetime-sentinel";}
static void *create(obs_data_t*d,obs_source_t*s){(void)d;os_atomic_inc_long(&created);return s;}
static void destroy(void*p){(void)p;os_atomic_inc_long(&destroyed);}
static void render(void*p,gs_effect_t*e){(void)p;(void)e;}
bool studio_snapshot_checkpoint(obs_source_t*raw){
 if(raw==target_raw&&os_atomic_compare_swap_long(&reader_armed,1,0)){
  os_event_signal(reader_gate);os_event_wait(release_gate);
  if(obs_weak_source_expired(target_weak)){os_atomic_store_long(&avoided_stale,1);return false;}
 }
 return true;
}
float studio_snapshot_transition_time(obs_source_t *transition){os_atomic_inc_long(&transition_time_calls);return obs_transition_get_time(transition);}
void studio_snapshot_writer_checkpoint(void){if(os_atomic_exchange_long(&writer_armed,0))os_event_signal(writer_gate);}
static void collect(obs_source_t*p,obs_source_t*c,void*d){(void)p;obs_source_t**r=d;if(!*r)*r=obs_source_get_ref(c);}
static obs_source_t *current(obs_source_t*s){obs_source_t*r=NULL;obs_source_enum_active_sources(s,collect,&r);return r;}
struct job{obs_source_t*source;obs_data_t*settings;obs_source_t*snapshot;};
static void *read_job(void*p){struct job*j=p;j->snapshot=studio_snapshot_get(j->source);return NULL;}
static void *write_job(void*p){struct job*j=p;obs_source_update(j->source,j->settings);studio_graph_tick(j->source);studio_graph_drain();return NULL;}
struct enum_job{obs_source_t*source;obs_data_t*settings;obs_weak_source_t*old;bool alive_inside;unsigned callbacks;};
static void reentrant_enum(obs_source_t*p,obs_source_t*c,void*d){
 struct enum_job*j=d;++j->callbacks;j->old=obs_source_get_weak_source(c);
 obs_source_update(j->source,j->settings);studio_graph_tick(j->source);studio_graph_drain();
 j->alive_inside=!obs_weak_source_expired(j->old);(void)p;
}
int main(int argc,char**argv){
 if(argc!=3||!(report=fopen(argv[1],"w")))return 90;bool fixed=!strcmp(argv[2],"after");
 check("setup real libobs startup",obs_startup("en-US",NULL,NULL));
 struct obs_source_info sentinel={.id="cut_transition",.type=OBS_SOURCE_TYPE_TRANSITION,.get_name=name,.create=create,.destroy=destroy,.video_render=render};
 obs_register_source(&sentinel);sentinel.id="fade_transition";obs_register_source(&sentinel);obs_register_source(&slideshow_info);obs_register_source(&slideshow_info_mk2);
 check("setup reader event",os_event_init(&reader_gate,OS_EVENT_TYPE_MANUAL)==0);check("setup release event",os_event_init(&release_gate,OS_EVENT_TYPE_MANUAL)==0);check("setup writer event",os_event_init(&writer_gate,OS_EVENT_TYPE_MANUAL)==0);
 obs_data_t*settings=obs_data_create();obs_data_set_string(settings,"transition","cut");obs_data_set_string(settings,"slide_mode","mode_manual");
 obs_source_t*source=obs_source_create_private("slideshow_v2","snapshot-owner",settings);check("setup real mk2 created",source&&obs_obj_get_data(source));studio_graph_tick(source);obs_source_inc_active(source);
 for(unsigned iteration=0;iteration<3;++iteration){
  fprintf(report,"ITERATION\t%u\n",iteration);target_raw=current(source);check("setup existing transition",target_raw!=NULL);target_weak=obs_source_get_weak_source(target_raw);obs_source_release(target_raw);
  os_atomic_store_long(&reader_armed,1);os_atomic_store_long(&writer_armed,1);os_atomic_store_long(&avoided_stale,0);os_event_reset(reader_gate);os_event_reset(release_gate);os_event_reset(writer_gate);
  struct job job={.source=source,.settings=settings};pthread_t reader,writer;
  check("setup reader thread",pthread_create(&reader,NULL,read_job,&job)==0);check("reader paused after raw load before get_ref",os_event_timedwait(reader_gate,2000)==0);
  obs_data_set_string(settings,"transition",iteration%2?"cut":"fade");check("setup writer thread",pthread_create(&writer,NULL,write_job,&job)==0);check("writer reaches actual transition publication",os_event_timedwait(writer_gate,2000)==0);
  if(!fixed){pthread_join(writer,NULL);studio_graph_drain();check("raw reader lifetime survives completed replacement",!obs_weak_source_expired(target_weak));os_event_signal(release_gate);pthread_join(reader,NULL);}
  else{os_event_signal(release_gate);pthread_join(reader,NULL);pthread_join(writer,NULL);studio_graph_drain();check("raw reader lifetime survives completed replacement",!obs_weak_source_expired(target_weak));}
  check("actual snapshot acquires strong old reference",job.snapshot!=NULL&&!obs_weak_source_expired(target_weak));
  check("fixture never needed to suppress stale get_ref",!os_atomic_load_long(&avoided_stale));
  obs_source_release(job.snapshot);studio_graph_drain();check("old transition retires after snapshot release",obs_weak_source_expired(target_weak));obs_weak_source_release(target_weak);target_weak=NULL;target_raw=NULL;
 }
 obs_data_set_string(settings,"transition","cut");obs_source_update(source,settings);studio_graph_tick(source);studio_graph_drain();obs_data_set_string(settings,"transition","fade");
 struct enum_job ej={.source=source,.settings=settings};obs_source_enum_active_sources(source,reentrant_enum,&ej);
 check("enum callback executed once",ej.callbacks==1);check("enum callback snapshot survives reentrant replacement",ej.alive_inside);
 studio_graph_drain();check("enum snapshot released after callback",ej.old&&obs_weak_source_expired(ej.old));obs_weak_source_release(ej.old);
 os_atomic_store_long(&transition_time_calls,0);obs_source_media_next(source);studio_graph_tick(source);check("empty next action does not query transition time",os_atomic_load_long(&transition_time_calls)==0);
 os_atomic_store_long(&transition_time_calls,0);obs_source_media_previous(source);studio_graph_tick(source);check("empty previous action does not query transition time",os_atomic_load_long(&transition_time_calls)==0);
 obs_source_dec_active(source);obs_source_release(source);obs_data_release(settings);studio_graph_drain();check("all sentinel transitions destroyed exactly once",os_atomic_load_long(&created)==os_atomic_load_long(&destroyed));
 os_event_destroy(reader_gate);os_event_destroy(release_gate);os_event_destroy(writer_gate);obs_shutdown();fprintf(report,"TOTAL\t%u\t%u\n",checks,failures);fclose(report);return failures?1:0;
}
