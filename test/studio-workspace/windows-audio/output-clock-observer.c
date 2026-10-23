#include "output-clock-observer.h"

#include <obs-internal.h>
#include <string.h>

static void copy_packet_clock(struct studio_packet_clock_sample *sample, const struct encoder_packet *packet)
{
	sample->present = 1;
	sample->keyframe = packet->keyframe ? 1u : 0u;
	sample->dts = packet->dts;
	sample->pts = packet->pts;
	sample->dts_usec = packet->dts_usec;
	sample->sys_dts_usec = packet->sys_dts_usec;
	sample->timebase_num = packet->timebase_num;
	sample->timebase_den = packet->timebase_den;
	sample->track_index = (uint64_t)packet->track_idx;
}

static void copy_encoder_clock(struct studio_encoder_clock_sample *sample, const struct obs_encoder *encoder)
{
	if (!encoder)
		return;
	sample->present = 1;
	sample->observational_only = 1;
	/* No encoder mutex is acquired under the output mutex. These fields are
	 * observations only; the output mutex does not synchronize their writers. */
	sample->active = encoder->active ? 1u : 0u;
	sample->first_received = encoder->first_received ? 1u : 0u;
	sample->start_ts_ns = encoder->start_ts;
	sample->first_raw_ts_ns = encoder->first_raw_ts;
	sample->cur_pts = encoder->cur_pts;
	sample->timebase_num = encoder->timebase_num;
	sample->timebase_den = encoder->timebase_den;
	sample->audio_input_buffer0_bytes = (uint64_t)encoder->audio_input_buffer[0].size;
	sample->frame_size_samples = (uint64_t)encoder->framesize;
	sample->sample_rate = encoder->samplerate;
}

int studio_output_clock_observe(struct obs_output *output, struct studio_output_clock_snapshot *snapshot)
{
	if (!snapshot)
		return STUDIO_OUTPUT_CLOCK_INVALID_ARGUMENT;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->abi_version = 1;
	snapshot->snapshot_bytes = (uint32_t)sizeof(*snapshot);
	snapshot->internal_output_bytes = (uint32_t)sizeof(struct obs_output);
	snapshot->internal_encoder_bytes = (uint32_t)sizeof(struct obs_encoder);
	snapshot->sampled_at_ns = os_gettime_ns();
	if (!output)
		return STUDIO_OUTPUT_CLOCK_INVALID_ARGUMENT;

	snapshot->lock_result = pthread_mutex_trylock(&output->interleaved_mutex);
	if (snapshot->lock_result != 0)
		return STUDIO_OUTPUT_CLOCK_LOCK_UNAVAILABLE;

	snapshot->packet_count = (uint64_t)output->interleaved_packets.num;
	snapshot->received_audio = output->received_audio ? 1u : 0u;
	snapshot->received_video0 = output->received_video[0] ? 1u : 0u;
	snapshot->highest_audio_ts = output->highest_audio_ts;
	snapshot->highest_video_ts0 = output->highest_video_ts[0];
	snapshot->video_offset0 = output->video_offsets[0];
	snapshot->audio_offset0 = output->audio_offsets[0];
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i)
		snapshot->attached_video_encoders += output->video_encoders[i] ? 1u : 0u;
	for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; ++i)
		snapshot->attached_audio_encoders += output->audio_encoders[i] ? 1u : 0u;

	for (size_t i = 0; i < output->interleaved_packets.num; ++i) {
		const struct encoder_packet *packet = &output->interleaved_packets.array[i];
		if (packet->track_idx != 0)
			continue;
		if (packet->type == OBS_ENCODER_VIDEO && !snapshot->first_video0.present)
			copy_packet_clock(&snapshot->first_video0, packet);
		else if (packet->type == OBS_ENCODER_AUDIO && !snapshot->first_audio0.present)
			copy_packet_clock(&snapshot->first_audio0, packet);
		if (snapshot->first_video0.present && snapshot->first_audio0.present)
			break;
	}
	for (size_t i = output->interleaved_packets.num; i > 0; --i) {
		const struct encoder_packet *packet = &output->interleaved_packets.array[i - 1];
		if (packet->track_idx != 0)
			continue;
		if (packet->type == OBS_ENCODER_VIDEO && !snapshot->last_video0.present)
			copy_packet_clock(&snapshot->last_video0, packet);
		else if (packet->type == OBS_ENCODER_AUDIO && !snapshot->last_audio0.present)
			copy_packet_clock(&snapshot->last_audio0, packet);
		if (snapshot->last_video0.present && snapshot->last_audio0.present)
			break;
	}
	copy_encoder_clock(&snapshot->video_encoder0, output->video_encoders[0]);
	copy_encoder_clock(&snapshot->audio_encoder0, output->audio_encoders[0]);
	snapshot->valid = 1;
	snapshot->unlock_result = pthread_mutex_unlock(&output->interleaved_mutex);
	return snapshot->unlock_result == 0 ? STUDIO_OUTPUT_CLOCK_OK : STUDIO_OUTPUT_CLOCK_UNLOCK_FAILED;
}
