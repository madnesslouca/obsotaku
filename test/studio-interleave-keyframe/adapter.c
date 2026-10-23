/* Private adapter appended to an exact copy of obs-output.c by the relinker.
 * Calls production static functions. No implementation of pruning is copied.
 * This object must replace obs-output.c.o only in a private, never installed DLL.
 */
#define STUDIO_INTERLEAVE_PROBE_CORE
#include "probe.h"
struct studio_interleave_probe {
 struct obs_output output;
 struct obs_encoder video[2], audio[2];
 struct caption_track_data caption[2];
 size_t peak_queued;
 DARRAY(struct studio_interleave_event) events;
};
static void studio_probe_sink(void *opaque, struct encoder_packet *packet)
{
 struct studio_interleave_probe *p = opaque;
 if (!packet) return;
 struct studio_interleave_event e = {
  .type = packet->type, .track = (int)packet->track_idx,
  .keyframe = packet->keyframe, .pts = packet->pts, .dts_usec = packet->dts_usec,
 };
 if (packet->size == sizeof(e.raw_index)) memcpy(&e.raw_index, packet->data, sizeof(e.raw_index));
 da_push_back(p->events, &e);
}
EXPORT void *studio_interleave_probe_create_multi(unsigned videos, unsigned tracks)
{
 if (!tracks || tracks > 2 || !videos || videos > 2) return NULL;
 struct studio_interleave_probe *p = bzalloc(sizeof(*p));
 struct obs_output *o = &p->output;
 pthread_mutex_init(&o->interleaved_mutex, NULL);
 pthread_mutex_init(&o->pkt_callbacks_mutex, NULL);
 for (unsigned i = 0; i < videos; ++i) {
  pthread_mutex_init(&p->caption[i].caption_mutex, NULL);
  o->caption_tracks[i] = &p->caption[i];
  p->video[i].info.type = OBS_ENCODER_VIDEO;
  p->video[i].timebase_num = 1; p->video[i].timebase_den = i ? 30 : 15;
  o->video_encoders[i] = &p->video[i];
 }
 o->context.name = "private-interleave-keyframe";
 o->context.data = p;
 o->info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
 o->info.encoded_packet = studio_probe_sink;
 o->active = true;
 o->data_active = true;
 o->interleaver_max_batch_size = 8;
 for (unsigned i = 0; i < tracks; ++i) {
  p->audio[i].info.type = OBS_ENCODER_AUDIO;
  p->audio[i].timebase_num = 1; p->audio[i].timebase_den = 48000;
  p->audio[i].framesize = 1024;
  o->audio_encoders[i] = &p->audio[i];
 }
 reset_packet_data(o);
 return p;
}
EXPORT void *studio_interleave_probe_create(unsigned tracks)
{ return studio_interleave_probe_create_multi(1, tracks); }
EXPORT long studio_interleave_probe_allocations(void) { return bnum_allocs(); }
EXPORT void studio_interleave_probe_set_active_delay(void *opaque, bool enabled)
{ struct studio_interleave_probe *p = opaque; p->output.active_delay_ns = enabled ? 1000000 : 0; }
EXPORT void studio_interleave_probe_feed_reordered(void *opaque, bool video, unsigned track,
                                        int64_t pts, int64_t dts, int64_t absolute_usec, bool keyframe)
{
 struct studio_interleave_probe *p = opaque;
 if (!p || track > 1 || (video ? !p->output.video_encoders[track] : !p->output.audio_encoders[track])) return;
 int64_t payload = pts;
 struct encoder_packet packet = {
  .data = (uint8_t *)&payload, .size = sizeof(payload), .type = video ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO,
  .timebase_num = 1, .timebase_den = video ? (track ? 30 : 15) : 48000,
  .dts = dts, .pts = pts, .dts_usec = absolute_usec, .sys_dts_usec = absolute_usec,
  .keyframe = keyframe, .encoder = video ? &p->video[track] : &p->audio[track],
 };
 struct encoder_packet_time timing = {.pts = pts};
 if (p->output.active_delay_ns) {
  /* process_delay_data pops the owning delayed packet and transfers it to
   * interleave_packets; it does not release it again after the callback. */
  struct encoder_packet owned;
  obs_encoder_packet_create_instance(&owned, &packet);
  interleave_packets(&p->output, &owned, video ? &timing : NULL);
 } else {
  interleave_packets(&p->output, &packet, video ? &timing : NULL);
 }
 if (p->output.interleaved_packets.num > p->peak_queued) p->peak_queued = p->output.interleaved_packets.num;
}
EXPORT void studio_interleave_probe_feed(void *opaque, bool video, unsigned track, int64_t pts, int64_t absolute_usec, bool keyframe)
{ studio_interleave_probe_feed_reordered(opaque, video, track, pts, pts, absolute_usec, keyframe); }
EXPORT void studio_interleave_probe_state(void *opaque, struct studio_interleave_state *state)
{
 struct studio_interleave_probe *p = opaque;
 *state = (struct studio_interleave_state){
  .emitted = p->events.num, .queued = p->output.interleaved_packets.num, .peak_queued = p->peak_queued,
  .video_offset = p->output.video_offsets[0],
  .audio_offsets = {p->output.audio_offsets[0], p->output.audio_offsets[1]},
  .video_offsets = {p->output.video_offsets[0], p->output.video_offsets[1]},
  .received_video = p->output.received_video[0], .received_audio = p->output.received_audio,
  .timing_matches_queued_video = true,
 };
 for (unsigned track = 0; track < 2; ++track) {
  state->timings[track] = p->output.encoder_packet_times[track].num;
  for (size_t i = 0; i < p->output.interleaved_packets.num; ++i) {
   struct encoder_packet *pkt = &p->output.interleaved_packets.array[i];
   if (pkt->type != OBS_ENCODER_VIDEO || pkt->track_idx != track) continue;
   ++state->queued_video[track]; bool found = false;
   for (size_t j = 0; j < state->timings[track]; ++j)
    if (p->output.encoder_packet_times[track].array[j].pts == pkt->pts) found = true;
   state->timing_matches_queued_video &= found;
  }
  state->timing_matches_queued_video &= state->queued_video[track] == state->timings[track];
 }
}
EXPORT bool studio_interleave_probe_event(void *opaque, size_t index, struct studio_interleave_event *event)
{
 struct studio_interleave_probe *p = opaque;
 if (index >= p->events.num) return false;
 *event = p->events.array[index]; return true;
}
EXPORT void studio_interleave_probe_reset(void *opaque)
{
 struct studio_interleave_probe *p = opaque;
 reset_packet_data(&p->output);
 da_clear(p->events);
 p->peak_queued = 0;
}
EXPORT void studio_interleave_probe_destroy(void *opaque)
{
 struct studio_interleave_probe *p = opaque;
 if (!p) return;
 reset_packet_data(&p->output);
 for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i) da_free(p->output.encoder_packet_times[i]);
 da_free(p->output.keyframe_group_tracking);
 da_free(p->output.pkt_callbacks);
 da_free(p->events);
 for (unsigned i = 0; i < 2; ++i) if (p->output.caption_tracks[i]) pthread_mutex_destroy(&p->caption[i].caption_mutex);
 pthread_mutex_destroy(&p->output.pkt_callbacks_mutex);
 pthread_mutex_destroy(&p->output.interleaved_mutex);
 bfree(p);
}
