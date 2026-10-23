#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct obs_output;

enum studio_output_clock_result {
	STUDIO_OUTPUT_CLOCK_OK = 0,
	STUDIO_OUTPUT_CLOCK_LOCK_UNAVAILABLE = 1,
	STUDIO_OUTPUT_CLOCK_INVALID_ARGUMENT = 2,
	STUDIO_OUTPUT_CLOCK_UNLOCK_FAILED = 3,
};

struct studio_packet_clock_sample {
	uint32_t present;
	uint32_t keyframe;
	int64_t dts;
	int64_t pts;
	int64_t dts_usec;
	int64_t sys_dts_usec;
	int32_t timebase_num;
	int32_t timebase_den;
	uint64_t track_index;
};

struct studio_encoder_clock_sample {
	uint32_t present;
	uint32_t observational_only;
	uint32_t active;
	uint32_t first_received;
	uint64_t start_ts_ns;
	uint64_t first_raw_ts_ns;
	int64_t cur_pts;
	uint32_t timebase_num;
	uint32_t timebase_den;
	uint64_t audio_input_buffer0_bytes;
	uint64_t frame_size_samples;
	uint32_t sample_rate;
	uint32_t reserved;
};

struct studio_output_clock_snapshot {
	uint32_t abi_version;
	uint32_t snapshot_bytes;
	uint32_t internal_output_bytes;
	uint32_t internal_encoder_bytes;
	uint32_t valid;
	int32_t lock_result;
	int32_t unlock_result;
	uint32_t encoder_fields_synchronized;
	uint64_t sampled_at_ns;
	uint64_t packet_count;
	uint32_t received_audio;
	uint32_t received_video0;
	int64_t highest_audio_ts;
	int64_t highest_video_ts0;
	int64_t video_offset0;
	int64_t audio_offset0;
	uint32_t attached_video_encoders;
	uint32_t attached_audio_encoders;
	struct studio_packet_clock_sample first_video0;
	struct studio_packet_clock_sample last_video0;
	struct studio_packet_clock_sample first_audio0;
	struct studio_packet_clock_sample last_audio0;
	struct studio_encoder_clock_sample video_encoder0;
	struct studio_encoder_clock_sample audio_encoder0;
};

/* QA-module-local helper, deliberately not an exported libobs API.
 *
 * Compile this C file against the EXACT running fork's obs-internal.h, generated
 * headers, architecture and dependency headers (including w32-pthreads on Win).
 * The caller must retain a strong output reference, serialize lifecycle and
 * encoder reassignment, and keep both attached slot-0 encoders alive. Never call
 * during shutdown/reconfiguration. Do not call while already holding the output
 * mutex: some pthread implementations allow recursive locking.
 *
 * A single pthread_mutex_trylock guards the queue and output clock fields;
 * failure returns without inspecting them. No waiting, second mutex, allocation,
 * packet reference, payload access, mutation, log or API callback is performed.
 * Finding first/last slot-0 packets can scan the queue: sample at a low rate.
 *
 * Encoder clock members have no common writer lock with this output mutex.
 * They are explicitly observational/racy, may represent different instants and
 * MUST NOT be treated as a coherent synchronized encoder snapshot. Zero is a
 * legitimate timestamp; use present flags. Queue DTS fields are copied as
 * stored, without recomputing or interpreting initialization/offset state.
 */
int studio_output_clock_observe(struct obs_output *output, struct studio_output_clock_snapshot *snapshot);

#ifdef __cplusplus
}
#endif
