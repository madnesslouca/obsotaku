#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
# ifdef STUDIO_INTERLEAVE_PROBE_CORE
#  define STUDIO_INTERLEAVE_API __declspec(dllexport)
# else
#  define STUDIO_INTERLEAVE_API __declspec(dllimport)
# endif
#else
# define STUDIO_INTERLEAVE_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct studio_interleave_event {
 int type, track;
 bool keyframe;
 int64_t pts, dts_usec, raw_index;
};
struct studio_interleave_state {
 size_t emitted, queued, peak_queued;
 int64_t video_offset, audio_offsets[2], video_offsets[2];
 bool received_video, received_audio, timing_matches_queued_video;
 size_t queued_video[2], timings[2];
};
STUDIO_INTERLEAVE_API void *studio_interleave_probe_create(unsigned audio_tracks);
STUDIO_INTERLEAVE_API void *studio_interleave_probe_create_multi(unsigned video_tracks, unsigned audio_tracks);
STUDIO_INTERLEAVE_API long studio_interleave_probe_allocations(void);
STUDIO_INTERLEAVE_API void studio_interleave_probe_set_active_delay(void *, bool enabled);
STUDIO_INTERLEAVE_API void studio_interleave_probe_feed(void *, bool video, unsigned track, int64_t pts,
                                 int64_t absolute_dts_usec, bool keyframe);
STUDIO_INTERLEAVE_API void studio_interleave_probe_feed_reordered(void *, bool video, unsigned track, int64_t pts, int64_t dts, int64_t absolute_dts_usec, bool keyframe);
STUDIO_INTERLEAVE_API void studio_interleave_probe_state(void *, struct studio_interleave_state *);
STUDIO_INTERLEAVE_API bool studio_interleave_probe_event(void *, size_t index, struct studio_interleave_event *);
STUDIO_INTERLEAVE_API void studio_interleave_probe_reset(void *);
STUDIO_INTERLEAVE_API void studio_interleave_probe_destroy(void *);
#ifdef __cplusplus
}
#endif
