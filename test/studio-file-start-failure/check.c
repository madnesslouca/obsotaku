/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static obs_data_t *healthy_settings,*failed_settings;
static int healthy_cookie,failed_cookie;
static obs_output_t *healthy=(obs_output_t *)&healthy_cookie,*failed=(obs_output_t *)&failed_cookie;
static int begin_calls,end_calls,signals,errors,opens,closes,writing_logs,threads_created,duplicate_closes,last_terminal_code;
static bool inject_packet_error,packet_error_injected;
static bool begin_ok,can_begin=true,encoders_ok=true,reentrant_stop;
static void *starting_flv;
struct opened {FILE *file;bool closed;};static struct opened files[16];
static bool fixture_begin(obs_output_t *out,uint32_t flags);
static bool fixture_can(const obs_output_t *out,uint32_t flags){(void)out;(void)flags;return can_begin;}
static bool fixture_enc(obs_output_t *out,uint32_t flags){(void)out;(void)flags;return encoders_ok;}
static void fixture_end(obs_output_t *out){(void)out;end_calls++;}
static void fixture_signal(obs_output_t *out,int code){(void)out;last_terminal_code=code;signals++;}
static void fixture_error(obs_output_t *out,const char *s){(void)out;if(s&&*s)errors++;}
static obs_data_t *fixture_settings(const obs_output_t *out){obs_data_t *s=out==healthy?healthy_settings:failed_settings;obs_data_addref(s);return s;}
static const char *fixture_name(const obs_output_t *out){return out==healthy?"healthy fixture":"failed fixture";}
static obs_encoder_t *fixture_audio(const obs_output_t *out,size_t idx){(void)out;(void)idx;return NULL;}
static obs_encoder_t *fixture_video(const obs_output_t *out,size_t idx){(void)out;(void)idx;return NULL;}
static FILE *fixture_open(const char *path,const char *mode){FILE *f=os_fopen(path,mode);if(f)files[opens++]=(struct opened){f,false};return f;}
static int fixture_close(FILE *f){for(int i=opens-1;i>=0;i--)if(files[i].file==f){if(files[i].closed){duplicate_closes++;return EOF;}files[i].closed=true;closes++;return fclose(f);}abort();}
static int fixture_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg){int rc=pthread_create(t,a,fn,arg);if(!rc)threads_created++;return rc;}
static void fixture_log(int level,const char *fmt,...){(void)level;if(strstr(fmt,"Writing FLV file"))writing_logs++;}
const char *obs_module_text(const char *s){return s;}
#define obs_output_begin_data_capture fixture_begin
#define obs_output_can_begin_data_capture fixture_can
#define obs_output_initialize_encoders fixture_enc
#define obs_output_end_data_capture fixture_end
#define obs_output_signal_stop fixture_signal
#define obs_output_set_last_error fixture_error
#define obs_output_get_settings fixture_settings
#define obs_output_get_name fixture_name
#define obs_output_get_audio_encoder fixture_audio
#define obs_output_get_video_encoder2 fixture_video
#define os_fopen fixture_open
#define fclose fixture_close
#define pthread_create fixture_create
#define blog fixture_log
#include TEST_NULL_SOURCE
#include TEST_FLV_SOURCE
#undef fclose
#undef pthread_create
#undef os_fopen
static bool fixture_begin(obs_output_t *out,uint32_t flags){(void)flags;begin_calls++;if(starting_flv&&reentrant_stop)flv_output_stop(starting_flv,0);if(starting_flv&&inject_packet_error){packet_error_injected=true;flv_output_info.encoded_packet(starting_flv,NULL);}return out==healthy||begin_ok;}
static FILE *result;static int checks,failures;
static void check(bool ok,const char *name){checks++;fprintf(result,"%s\t%s\n",ok?"PASS":"FAIL",name);if(!ok)failures++;}
static bool is_closed(FILE *f){for(int i=opens-1;i>=0;i--)if(files[i].file==f)return files[i].closed;return false;}
static void cleanup_flv(struct flv_output *s){if(s->file&&!is_closed(s->file))fixture_close(s->file);s->file=NULL;flv_output_destroy(s);}
int main(int argc,char **argv){
 if(argc!=5)return 2;
 if(strcmp(argv[4],"basic")&&strcmp(argv[4],"stop-refusal")&&strcmp(argv[4],"packet-error-refusal"))return 2;
 if(os_file_exists(argv[2])||os_file_exists(argv[3]))return 2;
 result=fopen(argv[1],"wx");if(!result)return 3;
 healthy_settings=obs_data_create();failed_settings=obs_data_create();obs_data_set_string(healthy_settings,"path",argv[2]);obs_data_set_string(failed_settings,"path",argv[3]);
 struct null_output *nhealthy=null_output_create(NULL,healthy),*nfailed=null_output_create(NULL,failed);
 check(null_output_start(nhealthy),"healthy null begins successfully");int b=begin_calls,e=end_calls,t=threads_created,err=errors;begin_ok=false;
 check(!null_output_start(nfailed),"null propagates explicit begin refusal");check(begin_calls==b+1,"null attempts begin once");check(errors==err+1,"null records refusal error");check(end_calls==e&&signals==0,"null refusal performs no core detach or terminal");check(threads_created==t&&!nfailed->stop_thread_active,"null refusal creates no stop worker");
 begin_ok=true;check(null_output_start(nfailed),"same null accepts later successful begin");
 null_output_stop(nfailed,0);pthread_t saved=nfailed->stop_thread;check(pthread_join(saved,NULL)==0,"owned successful null Stop worker joins natively");check(end_calls==e+1&&!nfailed->stop_thread_active,"successful null Stop ends once and clears worker flag");
 null_output_stop(nhealthy,0);saved=nhealthy->stop_thread;check(pthread_join(saved,NULL)==0,"second healthy null Stop remains functional");null_output_destroy(nfailed);null_output_destroy(nhealthy);
 struct flv_output *h=flv_output_create(NULL,healthy),*f=flv_output_create(NULL,failed);begin_ok=true;
 check(flv_output_start(h),"healthy FLV begins successfully");check(active(h)&&h->file,"healthy FLV owns open FILE");FILE *hf=h->file;
 b=begin_calls;e=end_calls;int sig=signals,c=closes,o=opens,w=writing_logs;err=errors;begin_ok=false;starting_flv=f;reentrant_stop=!strcmp(argv[4],"stop-refusal");inject_packet_error=!strcmp(argv[4],"packet-error-refusal");
 bool expected_packet_error=inject_packet_error;bool ok=flv_output_start(f);starting_flv=NULL;reentrant_stop=false;inject_packet_error=false;
 check(!ok,"FLV propagates explicit begin refusal");check(begin_calls==b+1,"FLV attempts begin once");check(!active(f)&&!stopping(f),"FLV refusal clears provisional flags");check(f->file==NULL,"FLV refusal clears owned FILE pointer");check(opens==o+1&&closes==c+1&&files[opens-1].closed,"FLV refusal closes exact own FILE once");check(errors==err+1,"FLV records refusal error");check(end_calls==e&&signals==sig+(expected_packet_error?1:0),"FLV refusal adds no core detach or duplicate terminal");check(packet_error_injected==expected_packet_error,"actual encoded_packet callback injected only in selected scenario");check(!expected_packet_error||last_terminal_code==OBS_OUTPUT_ENCODE_ERROR,"injected packet error preserves original encoded-error terminal");check(duplicate_closes==0,"cleanup never requests fclose on an already closed FILE");check(writing_logs==w,"FLV refusal emits no Writing success log");check(active(h)&&h->file==hf&&!is_closed(hf),"healthy FLV remains active with original FILE");check(fwrite("healthy survives",1,16,hf)==16&&fflush(hf)==0,"healthy FILE stays writable after other refusal");
 c=closes;int signal_after_decision=signals;flv_output_data(f,NULL);check(closes==c&&end_calls==e&&signals==signal_after_decision,"late packet on inactive refused FLV performs no cleanup twice");
 /* Before source falsely stays active; retire fixture-owned state without legacy terminal. */
 if(active(f)){os_atomic_set_bool(&f->active,false);if(f->file&&!is_closed(f->file))fixture_close(f->file);f->file=NULL;}
 begin_ok=true;check(flv_output_start(f),"same FLV accepts later successful begin");check(active(f)&&f->file,"successful retry owns active FILE");
 flv_output_stop(f,0);struct encoder_packet packet={0};packet.sys_dts_usec=1;flv_output_data(f,&packet);check(!active(f)&&end_calls==e+1,"successful retry Stop ends capture once");
 flv_output_stop(h,0);flv_output_data(h,&packet);check(!active(h)&&end_calls==e+2,"healthy FLV Stop still ends capture once");
 cleanup_flv(f);cleanup_flv(h);check(opens==closes,"all native owned FILE handles close exactly once");obs_data_release(healthy_settings);obs_data_release(failed_settings);
 fprintf(result,"TOTAL\t%d\tFAILURES\t%d\n",checks,failures);fclose(result);return failures?1:0;
}
