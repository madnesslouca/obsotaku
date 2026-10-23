/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact native init_send/send_thread implementation; explicit OBS begin refusal,
 * metadata serialization and RTMP codec are injected. Native threads, events,
 * semaphore and owned loopback TCP sockets remain real. No RTMP/media proof. */
#include <obs-module.h>
#include <util/threading.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET fixture_socket;
#define CLOSE_SOCKET closesocket
#define BAD_SOCKET INVALID_SOCKET
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int fixture_socket;
#define CLOSE_SOCKET close
#define BAD_SOCKET (-1)
#endif
#include "librtmp/rtmp.h"
static bool begin_result;
static int begin_calls,end_calls,signal_calls,error_calls;
static volatile long created,exited,joined,socket_exited,self_join,sender_exited,bad_join,detach_requests,break_wait;
static const char *scenario;
static struct rtmp_stream *starting_stream;
static bool checkpoint_reached,transport_intact_while_begin;
static bool fail_write;
static os_event_t *connect_release;
static void await_sender(void);
static long sender_target;
static int fixture_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
static int fixture_pthread_join(pthread_t,void **);
static bool fixture_begin(obs_output_t *,uint32_t);
static int fixture_wait(os_sem_t *);
static int fixture_detach(pthread_t);
static void fixture_end(obs_output_t *o){(void)o;end_calls++;}
static void fixture_signal(obs_output_t *o,int c){(void)o;(void)c;signal_calls++;}
static void fixture_error(obs_output_t *o,const char *s){(void)o;if(s&&*s)error_calls++;}
static const char *fixture_name(const obs_output_t *o){(void)o;return "RTMP-owned-fixture";}
static obs_encoder_t *fixture_video(const obs_output_t *o,size_t i){(void)o;(void)i;return NULL;}
static obs_encoder_t *fixture_audio(const obs_output_t *o,size_t i){(void)o;(void)i;return NULL;}
static void fixture_meta(obs_output_t *,uint8_t **,size_t *,bool);
const char *obs_module_text(const char *s){return s;}
int RTMP_Write(RTMP *r,const char *p,int n,int i){(void)i;if(fail_write)return -1;return (int)send(r->m_sb.sb_socket,p,n,0);}
void RTMP_Close(RTMP *r){if(r->m_sb.sb_socket!=BAD_SOCKET){CLOSE_SOCKET(r->m_sb.sb_socket);r->m_sb.sb_socket=BAD_SOCKET;}}
int RTMP_IsConnected(RTMP *r){return r->m_sb.sb_socket!=BAD_SOCKET;}
int RTMP_ReadPacket(RTMP *r,RTMPPacket *p){(void)r;(void)p;abort();return 0;}
void RTMPPacket_Free(RTMPPacket *p){(void)p;abort();}
#define obs_output_begin_data_capture fixture_begin
#define obs_output_end_data_capture fixture_end
#define obs_output_signal_stop fixture_signal
#define obs_output_set_last_error fixture_error
#define obs_output_get_name fixture_name
#define obs_output_get_video_encoder2 fixture_video
#define obs_output_get_audio_encoder fixture_audio
#define flv_meta_data fixture_meta
#define pthread_create fixture_pthread_create
#define pthread_join fixture_pthread_join
#define pthread_detach fixture_detach
#define os_sem_wait fixture_wait
#include TEST_PLUGIN_SOURCE
#undef pthread_create
#undef pthread_join
#undef pthread_detach
#undef os_sem_wait
#ifdef _WIN32
/* Controlled secondary worker: verifies join ownership only, not rtmp-windows.c. */
void *socket_thread_windows(void *data){struct rtmp_stream *s=data;os_event_wait(s->send_thread_signaled_exit);os_atomic_inc_long(&socket_exited);return NULL;}
#endif
struct task {void *(*fn)(void *);void *arg;};
struct tracked_thread {pthread_t id;bool joined,detached;volatile long finished;};
static struct tracked_thread threads[64];static int num_threads;
static struct tracked_thread *tracked(pthread_t t){for(int i=num_threads-1;i>=0;i--)if(pthread_equal(threads[i].id,t))return &threads[i];return NULL;}
static void *fixture_task(void *data){struct task t=*(struct task *)data;free(data);void *ret=t.fn(t.arg);os_atomic_set_long(&tracked(pthread_self())->finished,1);if(t.fn==send_thread)os_atomic_inc_long(&sender_exited);os_atomic_inc_long(&exited);return ret;}
static int fixture_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg){
#ifdef _WIN32
 if(!strcmp(scenario,"socket-fail")&&fn==socket_thread_windows){os_atomic_set_bool(&starting_stream->disconnected,true);os_atomic_set_long(&break_wait,1);os_sem_post(starting_stream->send_sem);await_sender();return EAGAIN;}
#endif
 struct task *task=malloc(sizeof(*task));*task=(struct task){fn,arg};int ret=pthread_create(t,a,fixture_task,task);if(ret)free(task);else{threads[num_threads++]=(struct tracked_thread){*t,false,false,false};os_atomic_inc_long(&created);}return ret;}
/* Record negative-control detach/invalid-join requests without executing an
 * invalid native join or losing the ability to retire the fixture's thread. */
static int fixture_detach(pthread_t t){struct tracked_thread *e=tracked(t);if(!e)abort();e->detached=true;os_atomic_inc_long(&detach_requests);return 0;}
static int fixture_pthread_join(pthread_t t,void **ret){struct tracked_thread *e=tracked(t);if(!strcmp(scenario,"connecting-stop")&&pthread_equal(t,pthread_self())){os_atomic_inc_long(&self_join);return EDEADLK;}if(!e||e->joined||e->detached){os_atomic_inc_long(&bad_join);return EINVAL;}if(pthread_equal(t,pthread_self())){os_atomic_inc_long(&self_join);return EDEADLK;}int rc=pthread_join(t,ret);if(!rc){e->joined=true;os_atomic_inc_long(&joined);}return rc;}
static void drain_fixture_threads(void){for(int i=0;i<num_threads;i++)if(!threads[i].joined){pthread_join(threads[i].id,NULL);threads[i].joined=true;os_atomic_inc_long(&joined);}}
static int fixture_wait(os_sem_t *sem){int ret=os_sem_wait(sem);long expected=1;if(!ret&&os_atomic_compare_exchange_long(&break_wait,&expected,0))return EIO;return ret;}
static void await_sender(void){uint64_t limit=os_gettime_ns()+2000000000ULL;while(os_atomic_load_long(&sender_exited)<sender_target&&os_gettime_ns()<limit)os_sleep_ms(1);checkpoint_reached=os_atomic_load_long(&sender_exited)>=sender_target;transport_intact_while_begin=starting_stream->rtmp.m_sb.sb_socket!=BAD_SOCKET;}
static void fixture_meta(obs_output_t *o,uint8_t **p,size_t *n,bool h){(void)o;(void)h;if(!strcmp(scenario,"meta-fail")||!strcmp(scenario,"meta-retire")){os_atomic_set_bool(&starting_stream->disconnected,true);os_atomic_set_long(&break_wait,1);os_sem_post(starting_stream->send_sem);await_sender();fail_write=!strcmp(scenario,"meta-fail");}*n=8;*p=bmemdup("metadata",8);}
static void *controlled_connect_worker(void *arg){os_event_wait(arg);return NULL;}
static bool fixture_begin(obs_output_t *o,uint32_t f){(void)o;(void)f;begin_calls++;if(!strcmp(scenario,"early-false")){os_atomic_set_bool(&starting_stream->disconnected,true);os_atomic_set_long(&break_wait,1);os_sem_post(starting_stream->send_sem);await_sender();}else if(!strcmp(scenario,"stop-true")||!strcmp(scenario,"connecting-stop")||!strcmp(scenario,"other-connecting-stop")){if(connect_release)os_event_signal(connect_release);rtmp_stream_stop(starting_stream,0);await_sender();}return begin_result;}
static FILE *result;static int checks,failures;
static void check(bool ok,const char *s){checks++;fprintf(result,"%s\t%s\n",ok?"PASS":"FAIL",s);if(!ok)failures++;}
static fixture_socket make_pair(fixture_socket *peer){
 fixture_socket listener=socket(AF_INET,SOCK_STREAM,0);if(listener==BAD_SOCKET)abort();struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;if(bind(listener,(void *)&a,sizeof(a))||listen(listener,1))abort();socklen_t n=sizeof(a);if(getsockname(listener,(void *)&a,&n))abort();fixture_socket client=socket(AF_INET,SOCK_STREAM,0);if(connect(client,(void *)&a,sizeof(a)))abort();*peer=accept(listener,NULL,NULL);CLOSE_SOCKET(listener);return client;
}
static void init(struct rtmp_stream *s,fixture_socket *peer){memset(s,0,sizeof(*s));s->output=(obs_output_t *)s;s->rtmp.m_sb.sb_socket=make_pair(peer);pthread_mutex_init(&s->packets_mutex,NULL);pthread_mutex_init(&s->dbr_mutex,NULL);os_event_init(&s->stop_event,OS_EVENT_TYPE_MANUAL);os_sem_init(&s->send_sem,0);pthread_mutex_init(&s->write_buf_mutex,NULL);os_event_init(&s->send_thread_signaled_exit,OS_EVENT_TYPE_MANUAL);os_event_init(&s->buffer_has_data_event,OS_EVENT_TYPE_AUTO);os_event_init(&s->buffer_space_available_event,OS_EVENT_TYPE_AUTO);os_event_init(&s->socket_available_event,OS_EVENT_TYPE_AUTO);}
static void cleanup(struct rtmp_stream *s,fixture_socket peer){struct tracked_thread *e=tracked(s->send_thread);if(e&&!e->joined&&!os_atomic_load_long(&e->finished)){s->stop_ts=0;os_event_signal(s->stop_event);os_sem_post(s->send_sem);}if(active(s)){s->stop_ts=0;os_event_signal(s->stop_event);os_sem_post(s->send_sem);fixture_pthread_join(s->send_thread,NULL);}if(e&&!e->joined){pthread_join(s->send_thread,NULL);e->joined=true;os_atomic_inc_long(&joined);}RTMP_Close(&s->rtmp);CLOSE_SOCKET(peer);os_event_destroy(s->stop_event);os_sem_destroy(s->send_sem);pthread_mutex_destroy(&s->packets_mutex);pthread_mutex_destroy(&s->dbr_mutex);deque_free(&s->packets);dstr_free(&s->path);pthread_mutex_destroy(&s->write_buf_mutex);os_event_destroy(s->send_thread_signaled_exit);os_event_destroy(s->buffer_has_data_event);os_event_destroy(s->buffer_space_available_event);os_event_destroy(s->socket_available_event);bfree(s->write_buf);}
int main(int argc,char **argv){if(argc!=3)return 2;scenario=argv[2];
 bool known=!strcmp(scenario,"basic")||!strcmp(scenario,"early-false")||!strcmp(scenario,"stop-true")||!strcmp(scenario,"meta-fail")||!strcmp(scenario,"meta-retire")||!strcmp(scenario,"connecting-stop")||!strcmp(scenario,"other-connecting-stop");
#ifdef _WIN32
 known=known||!strcmp(scenario,"socket-fail");
#else
 known=known||!strcmp(scenario,"unsupported");
#endif
 if(!known)return 2;
 result=fopen(argv[1],"wx");if(!result)return 3;
#ifdef _WIN32
 WSADATA w; if(WSAStartup(MAKEWORD(2,2),&w))return 4;
#endif
 int modes=1;
#ifdef _WIN32
 modes=2;
#endif
 for(int mode=0;mode<modes;mode++){
 if(!strcmp(scenario,"socket-fail")&&!mode)continue;
 struct rtmp_stream failed;fixture_socket peer;init(&failed,&peer);failed.new_socket_loop=mode!=0||!strcmp(scenario,"unsupported");begin_result=!strcmp(scenario,"stop-true")||!strcmp(scenario,"connecting-stop")||!strcmp(scenario,"other-connecting-stop");starting_stream=&failed;sender_target=os_atomic_load_long(&sender_exited)+1;checkpoint_reached=false;transport_intact_while_begin=false;int base_calls=begin_calls,base_end=end_calls,base_signals=signal_calls,base_errors=error_calls;long base_created=os_atomic_load_long(&created),base_exited=os_atomic_load_long(&exited),base_joined=os_atomic_load_long(&joined),base_socket=os_atomic_load_long(&socket_exited);int workers=1+mode;bool prebegin=!strcmp(scenario,"meta-fail")||!strcmp(scenario,"meta-retire")||!strcmp(scenario,"socket-fail")||!strcmp(scenario,"unsupported");if(!strcmp(scenario,"socket-fail"))workers=1;int expected_code=begin_result?OBS_OUTPUT_SUCCESS:((!strcmp(scenario,"meta-fail")||!strcmp(scenario,"meta-retire"))?OBS_OUTPUT_DISCONNECTED:OBS_OUTPUT_ERROR);
 if(!strcmp(scenario,"connecting-stop")){failed.connecting=true;failed.connect_thread=pthread_self();}
 if(!strcmp(scenario,"other-connecting-stop")){os_event_init(&connect_release,OS_EVENT_TYPE_MANUAL);fixture_pthread_create(&failed.connect_thread,NULL,controlled_connect_worker,connect_release);failed.connecting=true;workers++;}
 int ret=init_send(&failed);check(ret==expected_code,"RTMP init_send propagates begin result or completed Stop");check(begin_calls==base_calls+(prebegin?0:1),"RTMP attempts begin exactly once");check(!active(&failed),"refused RTMP no longer advertises active");check(os_atomic_load_long(&created)==base_created+workers&&os_atomic_load_long(&exited)==base_exited+workers&&os_atomic_load_long(&joined)==base_joined+workers,"refused sender exits and is joined before failure returns");check(failed.rtmp.m_sb.sb_socket==BAD_SOCKET,"refused RTMP owns no open transport socket");check(end_calls==base_end+(begin_result?1:0),"refused sender never calls end_data_capture; committed Stop ends once");check(signal_calls==base_signals,"refused sender leaves single terminal to connection owner");check(begin_result||prebegin?error_calls==base_errors:error_calls>base_errors,"explicit refusal records error; committed Stop does not invent one");
#ifdef _WIN32
 if(mode){check(os_atomic_load_long(&socket_exited)==base_socket+(!strcmp(scenario,"socket-fail")?0:1),"controlled Windows socket worker exits before refused start returns");check(!failed.socket_thread_active&&!failed.rtmp.m_bCustomSend,"refused Windows sender retires custom socket ownership");}
#else
 (void)base_socket;
#endif
 if(strcmp(scenario,"basic")&&strcmp(scenario,"unsupported")){check(checkpoint_reached,"sender reaches terminal while begin is still blocked");check(transport_intact_while_begin,"startup transport remains owned until begin decision");check(os_atomic_load_long(&detach_requests)==0,"precommit worker never requests detach");check(os_atomic_load_long(&bad_join)==0,"startup owner never attempts detached or missing thread join");}
 if(!strcmp(scenario,"other-connecting-stop")){check(tracked(failed.connect_thread)->joined,"Stop still joins a different connecting thread");os_event_destroy(connect_release);connect_release=NULL;}
 cleanup(&failed,peer);drain_fixture_threads();
 struct rtmp_stream good;init(&good,&peer);good.new_socket_loop=mode!=0;begin_result=true;const char *saved_scenario=scenario;scenario="basic";fail_write=false;long c=os_atomic_load_long(&created),j=os_atomic_load_long(&joined);base_end=end_calls;ret=init_send(&good);check(ret==OBS_OUTPUT_SUCCESS,"RTMP success control remains successful");check(active(&good),"successful RTMP publishes active");check(os_atomic_load_long(&created)==c+1+mode&&os_atomic_load_long(&joined)==j,"successful RTMP keeps its own sender running");check(good.rtmp.m_sb.sb_socket!=BAD_SOCKET,"successful RTMP keeps own transport");cleanup(&good,peer);check(end_calls==base_end+1,"normal successful stop still ends capture");check(os_atomic_load_long(&created)==os_atomic_load_long(&joined)&&os_atomic_load_long(&created)==os_atomic_load_long(&exited),"all owned sender threads joined after controls");check(signal_calls==base_signals,"controlled successes and refusals emit no duplicate terminals");check(os_atomic_load_long(&self_join)==0,"cleanup never attempts to join the calling thread");scenario=saved_scenario;
 }

#ifdef _WIN32
 WSACleanup();
#endif
 fprintf(result,"TOTAL\t%d\tFAILURES\t%d\n",checks,failures);fclose(result);return failures?1:0;}
