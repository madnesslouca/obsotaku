/* Exercises the actual production replay packet admission/purge implementation.
 * No OBS instance, encoder, mux subprocess or user configuration is opened. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "../../plugins/obs-ffmpeg/obs-ffmpeg-mux.c"
#include <stdio.h>

const char *obs_module_text(const char *text)
{
	return text;
}
obs_module_t *obs_current_module(void)
{
	return NULL;
}
static volatile bool finished;
static int64_t inspections;
static void *inspect_worker(void *ptr)
{
	struct ffmpeg_muxer *stream = ptr;
	while (!os_atomic_load_bool(&finished)) {
		calldata_t cd = {0};
		replay_buffer_status(stream, &cd);
		assert(calldata_int(&cd, "retained_bytes") >= 0);
		assert(calldata_int(&cd, "retained_bytes") <= calldata_int(&cd, "limit_bytes"));
		calldata_free(&cd);
		inspections++;
	}
	return NULL;
}
static void invariant(struct ffmpeg_muxer *stream)
{
	pthread_mutex_lock(&stream->replay_mutex);
	int64_t bytes = 0;
	int keyframes = 0;
	for (size_t i = 0; i < stream->packets.size / sizeof(struct encoder_packet); ++i) {
		struct encoder_packet *pkt = deque_data(&stream->packets, i * sizeof(*pkt));
		bytes += pkt->size;
		keyframes += pkt->type == OBS_ENCODER_VIDEO && pkt->keyframe;
		if (!i)
			assert(pkt->type == OBS_ENCODER_VIDEO && pkt->keyframe);
	}
	assert(bytes == stream->cur_size);
	assert(keyframes == stream->keyframes);
	assert(bytes >= 0 && bytes + stream->replay_snapshot_bytes <= stream->max_size);
	pthread_mutex_unlock(&stream->replay_mutex);
}
static void join_window_regression(struct ffmpeg_muxer *stream)
{
	struct encoder_packet packet = {.sys_dts_usec = 10000};
	stream->replay_request_pending = true;
	stream->replay_request_id = 42;
	stream->save_ts = 9000;
	stream->keyframes = 1;
	assert(replay_save_ready(stream, &packet, 42));
	// Model cancel while the packet thread has unlocked to join its prior worker.
	stream->replay_request_pending = false;
	stream->save_ts = 0;
	assert(!replay_save_ready(stream, &packet, 42));
	// A newer request must retain its own timestamp and never reuse the old packet.
	stream->replay_request_pending = true;
	stream->replay_request_id = 43;
	stream->save_ts = 10001;
	assert(!replay_save_ready(stream, &packet, 42));
	assert(!replay_save_ready(stream, &packet, 43));
	packet.sys_dts_usec = 10002;
	assert(!replay_save_ready(stream, &packet, 42));
	assert(replay_save_ready(stream, &packet, 43));
	os_atomic_set_bool(&stream->stopping, true);
	assert(!replay_save_ready(stream, &packet, 43));
	os_atomic_set_bool(&stream->stopping, false);
	os_atomic_set_bool(&stream->active, false);
	assert(!replay_save_ready(stream, &packet, 43));
	os_atomic_set_bool(&stream->active, true);
	stream->keyframes = 0;
	assert(!replay_save_ready(stream, &packet, 43));
	stream->keyframes = 1;
	os_atomic_set_bool(&stream->muxing, true);
	assert(!replay_save_ready(stream, &packet, 43));
	os_atomic_set_bool(&stream->muxing, false);
	stream->keyframes = 0;
	stream->replay_request_pending = false;
	stream->save_ts = 0;
}
static int long_gop_window_regression(void)
{
	struct ffmpeg_muxer stream = {0};
	pthread_mutex_init(&stream.replay_mutex, NULL);
	stream.replay_mutex_initialized = true;
	stream.max_size = 16 * 1024 * 1024;
	stream.max_time = 5000000;
	os_atomic_set_bool(&stream.active, true);
	os_atomic_set_bool(&stream.capturing, true);
	mux_packets_t retained = {0};
	int duration_checks = 0;
	unsigned char payload[8192] = {0};
	for (int frame = 0; frame < 900; ++frame) {
		struct encoder_packet packet = {0}, owned;
		packet.type = OBS_ENCODER_VIDEO;
		packet.keyframe = frame % 250 == 0;
		packet.dts_usec = packet.sys_dts_usec = frame * 1000000LL / 60;
		packet.pts = packet.dts = frame;
		packet.timebase_den = 60;
		packet.size = sizeof(payload);
		packet.data = payload;
		for (int audio = 0; audio < 2; ++audio) {
			packet.type = audio ? OBS_ENCODER_AUDIO : OBS_ENCODER_VIDEO;
			packet.keyframe = !audio && frame % 250 == 0;
			packet.size = audio ? 1024 : sizeof(payload);
			long *refs = bmalloc(packet.size + sizeof(long));
			*refs = 1;
			owned = packet;
			owned.data = (unsigned char *)(refs + 1);
			memcpy(owned.data, packet.data, packet.size);
			replay_buffer_data(&stream, &owned);
			obs_encoder_packet_release(&owned);
			invariant(&stream);
			if (frame >= 300) {
				assert(packet.dts_usec - stream.cur_time >= stream.max_time);
				duration_checks++;
			}
		}
		if (frame == 500) {
			/* Retain real packet references like the worker snapshot, while
			 * continuing admission. The 16MiB budget has room for both sets. */
			pthread_mutex_lock(&stream.replay_mutex);
			for (size_t offset = 0; offset < stream.packets.size; offset += sizeof(struct encoder_packet)) {
				struct encoder_packet ref;
				obs_encoder_packet_ref(&ref, deque_data(&stream.packets, offset));
				da_push_back(retained, &ref);
				stream.replay_snapshot_bytes += (int64_t)ref.size;
			}
			assert(stream.cur_size + stream.replay_snapshot_bytes <= stream.max_size);
			pthread_mutex_unlock(&stream.replay_mutex);
		}
	}
	assert(stream.replay_budget_resets == 0 && stream.replay_dropped_packets == 0);
	for (size_t i = 0; i < retained.num; ++i)
		obs_encoder_packet_release(&retained.array[i]);
	da_free(retained);
	stream.replay_snapshot_bytes = 0;
	replay_buffer_clear(&stream);
	pthread_mutex_destroy(&stream.replay_mutex);
	return duration_checks;
}
int main(void)
{
	const int duration_checks = long_gop_window_regression();
	struct ffmpeg_muxer stream = {0};
	pthread_mutex_init(&stream.replay_mutex, NULL);
	stream.replay_mutex_initialized = true;
	stream.max_size = 1024 * 1024;
	stream.max_time = 3000000;
	os_atomic_set_bool(&stream.active, true);
	os_atomic_set_bool(&stream.capturing, true);
	join_window_regression(&stream);
	stream.replay_request_pending = true;
	stream.replay_request_id = 42;
	stream.save_ts = 99;
	calldata_t cancel = {0};
	calldata_set_int(&cancel, "request_id", 41);
	cancel_replay_save(&stream, &cancel);
	assert(!calldata_bool(&cancel, "canceled") && stream.replay_request_pending && stream.save_ts == 99);
	calldata_clear(&cancel);
	cancel_replay_save(&stream, &cancel);
	assert(!calldata_bool(&cancel, "canceled") && stream.replay_request_pending && stream.save_ts == 99);
	calldata_free(&cancel);
	stream.replay_request_pending = false;
	stream.save_ts = 0;
	pthread_t observer;
	assert(pthread_create(&observer, NULL, inspect_worker, &stream) == 0);
	uint32_t seed = 817;
	unsigned char payload[65536] = {0};
	for (int i = 0; i < 50000; ++i) {
		seed = seed * 1664525u + 1013904223u;
		struct encoder_packet packet = {0}, owned;
		packet.type = (i % 3) ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
		/* Includes long 1,000-packet periods without any fresh keyframe. */
		packet.keyframe = packet.type == OBS_ENCODER_VIDEO && i % 1001 == 1;
		packet.size = 2048 + seed % 60000;
		packet.data = payload;
		packet.dts_usec = packet.sys_dts_usec = i * 16667LL;
		packet.pts = packet.dts = i;
		packet.timebase_den = 60;
		/* Model the worker retaining a snapshot while packet arrival continues. */
		if (i % 71 == 0) {
			pthread_mutex_lock(&stream.replay_mutex);
			stream.replay_snapshot_bytes = i % 142 ? stream.max_size / 2 : 0;
			while (stream.packets.size && stream.cur_size + stream.replay_snapshot_bytes > stream.max_size)
				purge(&stream);
			pthread_mutex_unlock(&stream.replay_mutex);
		}
		long *refs = bmalloc(packet.size + sizeof(long));
		*refs = 1;
		owned = packet;
		owned.data = (unsigned char *)(refs + 1);
		memcpy(owned.data, packet.data, packet.size);
		replay_buffer_data(&stream, &owned);
		obs_encoder_packet_release(&owned);
		invariant(&stream);
	}
	os_atomic_set_bool(&finished, true);
	pthread_join(observer, NULL);
	assert(stream.replay_dropped_packets > 0);
	assert(stream.replay_budget_resets > 0);
	const int64_t dropped = stream.replay_dropped_packets;
	const int64_t resets = stream.replay_budget_resets;
	os_atomic_set_bool(&stream.active, false);
	os_atomic_set_bool(&stream.capturing, false);
	stream.replay_snapshot_bytes = 0;
	replay_buffer_clear(&stream);
	assert(!stream.packets.size && !stream.cur_size && !stream.keyframes);
	calldata_t status = {0};
	replay_buffer_status(&stream, &status);
	assert(!calldata_bool(&status, "capturing") && calldata_int(&status, "retained_bytes") == 0 &&
	       calldata_int(&status, "limit_bytes") == 0);
	calldata_free(&status);
	pthread_mutex_destroy(&stream.replay_mutex);
	printf("{\"passed\":true,\"packets\":50000,\"invariants_per_packet\":4,\"long_gop_frames\":250,\"window_seconds\":5,\"long_gop_duration_checks\":%d,\"parallel_status_reads\":%lld,\"dropped_packets\":%lld,\"budget_resets\":%lld,\"scope\":\"actual production admission/purge; modeled snapshot payload; no encoder/mux/frontend\"}\n",
	       duration_checks, (long long)inspections, (long long)dropped, (long long)resets);
	return 0;
}
