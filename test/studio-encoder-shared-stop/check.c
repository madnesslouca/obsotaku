/* SPDX-License-Identifier: GPL-2.0-or-later
 * Real libobs video output, encoder dispatch and legacy stop path. The fixture
 * encoder buffers one frame so a timing entry remains in flight at detach.
 * No app, graphics context, device, codec, socket, profile or authentication.
 */
#include <obs-internal.h>
#include <media-io/video-frame.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct buffered_encoder { bool pending; int64_t pts; uint8_t payload; };
struct sink { pthread_mutex_t mutex; unsigned count; bool timed[16]; int64_t pts[16]; uint64_t cts[16]; };
/* These bridges exist only in a privately built adapter core. */
extern bool studio_test_encoder_initialize(obs_encoder_t *);
extern void studio_test_encoder_start(obs_encoder_t *, encoded_callback_t, void *);
extern void studio_test_encoder_stop(obs_encoder_t *, encoded_callback_t, void *);
static unsigned checks, failures;
static FILE *report;
static void check(const char *name, bool ok)
{
 ++checks; if (!ok) ++failures;
 fprintf(report, "%s\t%s\n", ok ? "PASS" : "FAIL", name); fflush(report);
}
static const char *name(void *unused) { UNUSED_PARAMETER(unused); return "Buffered dispatch fixture"; }
static void *create(obs_data_t *settings, obs_encoder_t *encoder)
{ UNUSED_PARAMETER(settings); UNUSED_PARAMETER(encoder); return bzalloc(sizeof(struct buffered_encoder)); }
static void destroy(void *data) { bfree(data); }
static bool encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
 struct buffered_encoder *e = data;
 *received = e->pending;
 if (*received) {
  packet->type = OBS_ENCODER_VIDEO; packet->pts = packet->dts = e->pts;
  packet->keyframe = true; packet->data = &e->payload; packet->size = 1;
 }
 e->pending = true; e->pts = frame->pts; e->payload = (uint8_t)frame->pts;
 return true;
}
static void packet(void *data, struct encoder_packet *p, struct encoder_packet_time *t)
{
 struct sink *s = data; pthread_mutex_lock(&s->mutex);
 unsigned i = s->count++;
 if (i < 16) { s->timed[i] = t != NULL; s->pts[i] = p->pts; s->cts[i] = t ? t->cts : 0; }
 pthread_mutex_unlock(&s->mutex);
}
static unsigned count(struct sink *s)
{ pthread_mutex_lock(&s->mutex); unsigned n = s->count; pthread_mutex_unlock(&s->mutex); return n; }
static bool wait_count(struct sink *s, unsigned n)
{
 uint64_t until = os_gettime_ns() + 2000000000ULL;
 while (count(s) < n && os_gettime_ns() < until) os_sleep_ms(1);
 return count(s) == n;
}
static bool push(video_t *video, uint64_t timestamp)
{
 struct video_frame frame;
 if (!video_output_lock_frame(video, &frame, 1, timestamp)) return false;
 for (size_t p = 0; p < 3; ++p) memset(frame.data[p], 32, frame.linesize[p] * (p ? 32 : 64));
 video_output_unlock_frame(video); return true;
}
static bool timing(struct sink *s, unsigned index, int64_t pts, uint64_t cts)
{
 pthread_mutex_lock(&s->mutex);
 bool ok = s->count > index && s->timed[index] && s->pts[index] == pts && s->cts[index] == cts;
 pthread_mutex_unlock(&s->mutex); return ok;
}
int main(int argc, char **argv)
{
 if (argc != 2 || !(report = fopen(argv[1], "w"))) return 90;
 bool ready = obs_startup("en-US", NULL, NULL);
 check("setup isolated libobs startup", ready); if (!ready) return 91;
 struct obs_encoder_info info = {.id="shared-stop-buffered", .type=OBS_ENCODER_VIDEO,
  .codec="h264", .get_name=name, .create=create, .destroy=destroy, .encode=encode};
 obs_register_encoder(&info);
 struct video_output_info voi = {.name="private-no-device", .format=VIDEO_FORMAT_I420,
  .fps_num=30, .fps_den=1, .width=64, .height=64, .cache_size=4,
  .colorspace=VIDEO_CS_709, .range=VIDEO_RANGE_PARTIAL};
 video_t *video = NULL;
 check("setup real video output", video_output_open(&video, &voi) == 0);
 obs_encoder_t *encoder = obs_video_encoder_create(info.id,"shared-stop",NULL,NULL);
 check("setup real encoder object", encoder != NULL);
 if (!video || !encoder) return 92;
 obs_encoder_set_video(encoder, video);
 check("setup real encoder initialized", studio_test_encoder_initialize(encoder));
 struct sink a = {0}, b = {0}, absent = {0};
 pthread_mutex_init(&a.mutex,NULL); pthread_mutex_init(&b.mutex,NULL);
 studio_test_encoder_start(encoder,packet,&a); studio_test_encoder_start(encoder,packet,&b);
 check("two callbacks share active encoder", obs_encoder_active(encoder) && encoder->callbacks.num == 2);
 const uint64_t base = 9000000000ULL, step = 33333333;
 check("publish buffered frame zero", push(video,base));
 check("publish frame one", push(video,base+step));
 check("real callback A receives first packet", wait_count(&a,1));
 check("real callback B receives first packet", wait_count(&b,1));
 studio_test_encoder_stop(encoder,packet,&b); /* serializes both callbacks; producer idle */
 check("nonfinal legacy stop preserves active input", obs_encoder_active(encoder) && encoder->callbacks.num == 1);
 check("nonfinal legacy stop retains in-flight timing", encoder->encoder_packet_times.num == 1);
 check("publish after nonfinal stop", push(video,base+2*step));
 check("remaining callback A advances", wait_count(&a,2));
 check("remaining A receives original frame-one timing", timing(&a,1,1,base+step));
 check("detached B receives no later packet", count(&b) == 1);
 studio_test_encoder_stop(encoder,packet,&absent);
 check("absent legacy stop retains in-flight timing", encoder->encoder_packet_times.num == 1);
 check("absent stop retains active input and callback", obs_encoder_active(encoder) && encoder->callbacks.num == 1);
 check("publish after absent stop", push(video,base+3*step));
 check("A advances after absent stop", wait_count(&a,3));
 check("A receives original frame-two timing", timing(&a,2,2,base+2*step));
 studio_test_encoder_start(encoder,packet,&a);
 check("duplicate start retains exactly one callback", encoder->callbacks.num == 1);
 check("duplicate start retains pending timing", encoder->encoder_packet_times.num == 1);
 check("publish after duplicate start", push(video,base+4*step));
 check("A advances once after duplicate start", wait_count(&a,4));
 check("A receives original frame-three timing", timing(&a,3,3,base+3*step));
 studio_test_encoder_stop(encoder,packet,&a);
 check("final legacy stop disconnects input", !obs_encoder_active(encoder) && encoder->callbacks.num == 0);
 check("final legacy stop clears timing after disconnect", encoder->encoder_packet_times.num == 0);
 check("final stop prevents duplicate delivery", count(&a) == 4 && count(&b) == 1);
 check("reinitialize after final stop", studio_test_encoder_initialize(encoder));
 studio_test_encoder_start(encoder,packet,&a);
 check("restart activates single callback", obs_encoder_active(encoder) && encoder->callbacks.num == 1);
 check("publish restart frame zero", push(video,base+10*step));
 check("publish restart frame one", push(video,base+11*step));
 check("restart callback advances", wait_count(&a,5));
 check("restart has fresh PTS and composition time", timing(&a,4,0,base+10*step));
 studio_test_encoder_stop(encoder,packet,&a);
 check("restart final stop drains and clears", !obs_encoder_active(encoder) && encoder->encoder_packet_times.num == 0);
 obs_encoder_release(encoder); video_output_close(video);
 pthread_mutex_destroy(&a.mutex); pthread_mutex_destroy(&b.mutex);
 obs_shutdown();
 fprintf(report,"TOTAL\t%u\t%u\n", checks,failures); fclose(report);
 return failures ? 1 : 0;
}
