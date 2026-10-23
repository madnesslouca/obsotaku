/* SPDX-License-Identifier: GPL-2.0-or-later
 * Compile both real slideshow implementations. Synthetic empty transition
 * backends isolate child ownership; no image/codec/GPU/device is exercised.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <stdio.h>
#include <stdlib.h>
extern struct obs_source_info slideshow_info, slideshow_info_mk2;
extern long studio_graph_show(obs_source_t *);
extern long studio_graph_active(obs_source_t *);
extern void studio_graph_tick(obs_source_t *);
extern void studio_graph_drain(void);
static unsigned checks, failures, created, destroyed;
static FILE *report;
const char *obs_module_text(const char *text) { return text; }
bool obs_module_get_string(const char *text, const char **translated) { *translated=text; return true; }
obs_module_t *obs_current_module(void) { return NULL; }
uint64_t image_source_get_memory_usage(void *data) { UNUSED_PARAMETER(data); abort(); }
void image_source_preload_image(void *data) { UNUSED_PARAMETER(data); abort(); }
static void check(const char *name, bool ok)
{ ++checks; if (!ok) ++failures; fprintf(report,"%s\t%s\n",ok?"PASS":"FAIL",name); fflush(report); }
static const char *name(void *data) { UNUSED_PARAMETER(data); return "Lifecycle sentinel"; }
static void *create(obs_data_t *settings, obs_source_t *source)
{ UNUSED_PARAMETER(settings); ++created; return source; }
static void destroy(void *data) { UNUSED_PARAMETER(data); ++destroyed; }
static void render(void *data, gs_effect_t *effect) { UNUSED_PARAMETER(data); UNUSED_PARAMETER(effect); }
static void collect(obs_source_t *parent, obs_source_t *child, void *data)
{ UNUSED_PARAMETER(parent); obs_source_t **result=data; if (!*result) *result=obs_source_get_ref(child); }
static obs_source_t *transition(obs_source_t *source)
{ obs_source_t *result=NULL; obs_source_enum_active_sources(source,collect,&result); return result; }
int main(int argc,char **argv)
{
 if(argc!=2 || !(report=fopen(argv[1],"w")))return 90;
 check("setup private libobs startup",obs_startup("en-US",NULL,NULL));
 struct obs_source_info sentinel={.id="cut_transition",.type=OBS_SOURCE_TYPE_TRANSITION,
 .get_name=name,.create=create,.destroy=destroy,.video_render=render};
 obs_register_source(&sentinel);sentinel.id="fade_transition";obs_register_source(&sentinel);
 obs_register_source(&slideshow_info);obs_register_source(&slideshow_info_mk2);
 const char *ids[]={"slideshow","slideshow_v2"};
 for(unsigned version=0;version<2;++version){
  fprintf(report,"VERSION\t%s\n",ids[version]);
  obs_data_t *settings=obs_data_create();obs_data_set_string(settings,"transition","cut");
  obs_data_set_string(settings,"slide_mode","mode_manual");
  obs_source_t *source=obs_source_create_private(ids[version],"private-empty-slideshow",settings);
  check("setup real slideshow created",source!=NULL);if(!source)return 91;
  studio_graph_tick(source);
  obs_source_t *current=transition(source);check("setup transition registered",current!=NULL);if(!current)return 92;
  obs_source_inc_active(source);
  check("initial transition inherits one active credit",studio_graph_active(current)==1&&studio_graph_show(current)==1);
  obs_source_release(current);
  for(unsigned iteration=0;iteration<6;++iteration){
   current=transition(source);obs_weak_source_t *old=obs_source_get_weak_source(current);
   obs_source_release(current);
   obs_data_set_string(settings,"transition",iteration%2?"cut":"fade");
   obs_source_update(source,settings);studio_graph_tick(source);studio_graph_drain();
   current=transition(source);check("replacement transition remains registered",current!=NULL);
   check("replacement gets exactly one active/show credit",current&&studio_graph_active(current)==1&&studio_graph_show(current)==1);
   check("old transition is released after replacement",obs_weak_source_expired(old));
   obs_weak_source_release(old);obs_source_release(current);
  }
  current=transition(source);obs_weak_source_t *last=obs_source_get_weak_source(current);
  obs_source_dec_active(source);
  check("deactivate returns current transition credits",studio_graph_active(current)==0&&studio_graph_show(current)==0);
  obs_source_release(current);obs_source_release(source);obs_data_release(settings);studio_graph_drain();
  check("last transition is released on slideshow destruction",obs_weak_source_expired(last));
  obs_weak_source_release(last);
  check("all sentinel transitions destroyed once",created==destroyed);
 }
 obs_shutdown();fprintf(report,"TOTAL\t%u\t%u\n",checks,failures);fclose(report);return failures?1:0;
}
