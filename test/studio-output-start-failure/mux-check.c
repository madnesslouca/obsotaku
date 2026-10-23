/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native exact plugin TU; only OBS preflight/begin and metadata are injected.
 * The process pipe, settings, mutex, deque and plugin cleanup remain real.
 * No encoded media/core attach transaction/Windows/service acceptance. */
#include <obs-module.h>
#include <util/pipe.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
static obs_data_t *settings;
static bool begin_result;
static int begin_calls,end_calls,signal_calls,error_calls,pipes_created,pipes_destroyed;
static const char *helper_program,*marker_root;
static char markers[16][2048];
static int healthy_cookie,failing_cookie;
static obs_output_t *failing_output=(obs_output_t *)&failing_cookie;
static obs_output_t *healthy_output=(obs_output_t *)&healthy_cookie;
const char *obs_module_text(const char *key){return key;}
const char *fixture_obs_output_get_name(const obs_output_t *out){return out==healthy_output?"healthy fixture":"failing fixture";}
obs_encoder_t *fixture_obs_output_get_video_encoder(const obs_output_t *out){(void)out;return NULL;}
obs_encoder_t *fixture_obs_output_get_audio_encoder(const obs_output_t *out,size_t i){(void)out;(void)i;return NULL;}
obs_data_t *fixture_obs_output_get_settings(const obs_output_t *out){(void)out;obs_data_addref(settings);return settings;}
bool fixture_obs_output_can_begin_data_capture(const obs_output_t *out,uint32_t flags){(void)out;(void)flags;return true;}
bool fixture_obs_output_initialize_encoders(obs_output_t *out,uint32_t flags){(void)out;(void)flags;return true;}
bool fixture_obs_output_begin_data_capture(obs_output_t *out,uint32_t flags){(void)flags;begin_calls++;return out==healthy_output||begin_result;}
void fixture_obs_output_end_data_capture(obs_output_t *out){(void)out;end_calls++;}
void fixture_obs_output_signal_stop(obs_output_t *out,int code){(void)out;(void)code;signal_calls++;}
void fixture_obs_output_set_last_error(obs_output_t *out,const char *error){(void)out;if(error&&*error)error_calls++;}
static bool marker_exists(int index){FILE *f=fopen(markers[index],"rb");if(!f)return false;fclose(f);return true;}
static os_process_pipe_t *fixture_pipe_create(const os_process_args_t *ignored,const char *type){
 (void)ignored;os_process_args_t *args=os_process_args_create(helper_program);
 snprintf(markers[pipes_created+1],sizeof(markers[0]),"%s-helper-%d.done",marker_root,pipes_created+1);
 os_process_args_add_arg(args,markers[pipes_created+1]);os_process_pipe_t *pipe=os_process_pipe_create2(args,type);os_process_args_destroy(args);if(pipe)pipes_created++;return pipe;
}
static int fixture_pipe_destroy(os_process_pipe_t *pipe){if(pipe)pipes_destroyed++;return os_process_pipe_destroy(pipe);}
#define obs_output_get_name fixture_obs_output_get_name
#define obs_output_get_video_encoder fixture_obs_output_get_video_encoder
#define obs_output_get_audio_encoder fixture_obs_output_get_audio_encoder
#define obs_output_get_settings fixture_obs_output_get_settings
#define obs_output_can_begin_data_capture fixture_obs_output_can_begin_data_capture
#define obs_output_initialize_encoders fixture_obs_output_initialize_encoders
#define obs_output_begin_data_capture fixture_obs_output_begin_data_capture
#define obs_output_end_data_capture fixture_obs_output_end_data_capture
#define obs_output_signal_stop fixture_obs_output_signal_stop
#define obs_output_set_last_error fixture_obs_output_set_last_error
#define os_process_pipe_create2 fixture_pipe_create
#define os_process_pipe_destroy fixture_pipe_destroy
#include TEST_PLUGIN_SOURCE
static FILE *result;
static int checks,failures;
static void check(bool pass,const char *name){checks++;fprintf(result,"%s\t%s\n",pass?"PASS":"FAIL",name);if(!pass)failures++;}
static void free_mux_fields(struct ffmpeg_muxer *s){if(s->pipe){os_process_pipe_destroy(s->pipe);s->pipe=NULL;}dstr_free(&s->path);dstr_free(&s->printable_path);dstr_free(&s->muxer_settings);dstr_free(&s->stream_key);}
int main(int argc,char **argv){
 if(argc!=4)return 2;
 if(!os_file_exists(argv[3])||os_file_exists(argv[2]))return 2;
 /* Every process marker and media probe belongs to this fresh invocation. */
 for(int i=1;i<16;i++){char marker[2048];snprintf(marker,sizeof(marker),"%s-helper-%d.done",argv[1],i);if(os_file_exists(marker))return 2;}
 FILE *probe=fopen(argv[2],"wx");if(!probe)return 2;fclose(probe);if(remove(argv[2]))return 2;
 helper_program=argv[3];marker_root=argv[1];result=fopen(argv[1],"wx");if(!result)return 3;settings=obs_data_create();obs_data_set_string(settings,"path",argv[2]);obs_data_set_int(settings,"max_time_sec",15);obs_data_set_int(settings,"max_size_mb",10);
 struct ffmpeg_muxer healthy={0};healthy.output=healthy_output;begin_result=true;
 check(ffmpeg_mux_start_internal(&healthy,settings),"healthy control mux accepts capture");
 check(healthy.pipe&&active(&healthy)&&capturing(&healthy),"healthy control owns process and capture flags");
 struct ffmpeg_muxer failed={0};failed.output=failing_output;begin_result=false;
 int b=begin_calls,e=end_calls,signals=signal_calls,errors=error_calls,created=pipes_created,destroyed=pipes_destroyed;
 bool ok=ffmpeg_mux_start_internal(&failed,settings);
 check(!ok,"mux propagates explicit begin refusal");check(begin_calls==b+1,"mux attempts begin exactly once");check(!active(&failed)&&!capturing(&failed)&&!stopping(&failed),"mux refuses provisional flags");check(failed.pipe==NULL,"mux retires only refused helper pipe");check(marker_exists(2),"refused helper completed after EOF and join");check(!marker_exists(1),"healthy helper remains open during unrelated refusal");check(pipes_created==created+1&&pipes_destroyed==destroyed+1,"mux refused helper process is joined once");check(error_calls==errors+1,"mux records capture refusal error");check(end_calls==e,"mux refusal never calls end_data_capture");check(signal_calls==signals,"synchronous mux refusal emits no duplicate stop");check(healthy.pipe&&active(&healthy)&&capturing(&healthy),"healthy mux remains active after other refusal");
 const uint8_t payload[]="healthy process survives unrelated refusal\n";check(os_process_pipe_write(healthy.pipe,payload,sizeof(payload)-1)==sizeof(payload)-1,"healthy process still accepts bytes after other refusal");
 free_mux_fields(&failed);
 struct ffmpeg_muxer replay={0};replay.output=failing_output;pthread_mutex_init(&replay.replay_mutex,NULL);replay.replay_mutex_initialized=true;struct encoder_packet packet={0};packet.pts=12345;deque_push_back(&replay.packets,&packet,sizeof(packet));replay.cur_size=77;dstr_copy(&replay.replay_last_path,"preserved-last-replay.mkv");b=begin_calls;e=end_calls;signals=signal_calls;errors=error_calls;created=pipes_created;
 ok=replay_buffer_start(&replay);check(!ok,"replay propagates explicit begin refusal");check(begin_calls==b+1,"replay attempts begin exactly once");check(!active(&replay)&&!capturing(&replay)&&!stopping(&replay),"replay refuses provisional flags");check(replay.packets.size==sizeof(packet)&&replay.cur_size==77,"replay refusal preserves retained ring");check(!strcmp(replay.replay_last_path.array,"preserved-last-replay.mkv"),"replay refusal preserves previous saved path");check(pipes_created==created,"replay refusal spawns no helper");check(error_calls==errors+1,"replay records capture refusal error");check(end_calls==e&&signal_calls==signals,"replay refusal performs no core detach or duplicate terminal");check(healthy.pipe&&active(&healthy),"healthy mux unaffected by refused replay");
 begin_result=true;check(replay_buffer_start(&replay),"replay success control accepts capture");check(active(&replay)&&capturing(&replay),"replay success control publishes flags");
 os_atomic_set_bool(&replay.active,false);os_atomic_set_bool(&replay.capturing,false);deque_free(&replay.packets);dstr_free(&replay.replay_last_path);pthread_mutex_destroy(&replay.replay_mutex);free_mux_fields(&healthy);obs_data_release(settings);check(pipes_created==pipes_destroyed,"all owned helper processes retired after fixture cleanup");check(marker_exists(1)&&marker_exists(2),"all owned helpers completed before pipe destroy returned");fprintf(result,"TOTAL\t%d\tFAILURES\t%d\n",checks,failures);fclose(result);return failures?1:0;
}
