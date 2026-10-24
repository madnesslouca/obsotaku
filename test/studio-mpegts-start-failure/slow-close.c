/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Whole MPEGTS TU; native threads/locks/AV contexts/AVIO files.
 * OBS metadata/begin and AVIO URL->owned-file path are injected. No media proof. */
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
static os_event_t *heavy_entered, *heavy_release, *late_finished, *healthy_finished, *healthy_written;
static _Thread_local int own_data_depth;
static volatile long disposal_lock_depth, healthy_write_result;
static bool heavy_gate, gate_avio;
static int fixture_interleaved(AVFormatContext *, AVPacket *);
static void heavy_checkpoint(void)
{
	os_atomic_set_long(&disposal_lock_depth, own_data_depth);
	os_event_signal(heavy_entered);
	if (os_event_timedwait(heavy_release, 3000))
		abort();
}
struct ffmpeg_output;
static struct ffmpeg_output *starting, *healthy_stream;
static const char *mode, *prefix;
static bool begin_result, fail_extra;
static volatile long invalid_metadata, late_calls, late_lock_attempts;
static bool late_retirement_gate, late_callback_running;
static os_event_t *late_attempted;
static pthread_t late_callback_owner, late_callback_thread;
static void *late_callback(void *);
static bool late_av_available(void);
static volatile long begin_calls, end_calls, signals, error_calls, wait_error, self_joins, bad_joins,
	concurrent_av_free;
static os_event_t *control_release, *metadata_entered, *metadata_release, *metadata_finished, *retirement_checkpoint;
static volatile bool metadata_running;
static bool metadata_gate, metadata_forwarded, observer_live_context, observer_writer_exited;
static pthread_t metadata_owner, metadata_thread, observer_thread;
static volatile long retirement_kind, native_header_completed;
static void metadata_checkpoint(void);
static void guarded_metadata_retirement(void);
static int fixture_lock(pthread_mutex_t *);
static int fixture_unlock(pthread_mutex_t *);
static int fixture_write_header(AVFormatContext *, AVDictionary **);
static void fixture_codec_free(AVCodecContext **);

static int last_code;
static long signals_in_begin;
static bool worker_checkpoint, context_live_at_checkpoint;
static int video_cookie, audio_cookie, service_cookie, video_io_cookie;
static obs_encoder_t *video_encoder = (obs_encoder_t *)&video_cookie, *audio_encoder = (obs_encoder_t *)&audio_cookie;
static struct video_output_info voi = {.format = VIDEO_FORMAT_I420,
				       .fps_num = 15,
				       .fps_den = 1,
				       .width = 320,
				       .height = 180,
				       .colorspace = VIDEO_CS_709,
				       .range = VIDEO_RANGE_PARTIAL};
static bool fixture_begin(obs_output_t *, uint32_t);
static int fixture_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static int fixture_join(pthread_t, void **);
static int fixture_wait(os_sem_t *);
static void fixture_signal(obs_output_t *, int);
static void fixture_end(obs_output_t *o)
{
	(void)o;
	os_atomic_inc_long(&end_calls);
}
static void fixture_error(obs_output_t *o, const char *s)
{
	(void)o;
	if (s && *s)
		os_atomic_inc_long(&error_calls);
}
static bool fixture_can(const obs_output_t *o, uint32_t flags)
{
	(void)o;
	(void)flags;
	return true;
}
static bool fixture_init_encoders(obs_output_t *o, uint32_t flags)
{
	(void)o;
	(void)flags;
	return true;
}
static obs_service_t *fixture_service(const obs_output_t *o)
{
	(void)o;
	return (obs_service_t *)&service_cookie;
}
static const char *fixture_connect_info(const obs_service_t *s, uint32_t type)
{
	(void)s;
	return type == OBS_SERVICE_CONNECT_INFO_SERVER_URL ? "udp://127.0.0.1:9" : "";
}
static uint32_t fixture_width(const obs_output_t *o)
{
	(void)o;
	return 320;
}
static uint32_t fixture_height(const obs_output_t *o)
{
	(void)o;
	return 180;
}
static obs_encoder_t *fixture_video(const obs_output_t *o)
{
	(void)o;
	return video_encoder;
}
static obs_encoder_t *fixture_audio(const obs_output_t *o, size_t i)
{
	(void)o;
	return i ? NULL : audio_encoder;
}
static const char *fixture_codec(const obs_encoder_t *e)
{
	return e == video_encoder ? "h264" : "aac";
}
static video_t *fixture_encoder_video(const obs_encoder_t *e)
{
	(void)e;
	return (video_t *)&video_io_cookie;
}
static enum video_format fixture_format(const video_t *v)
{
	(void)v;
	return voi.format;
}
static const struct video_output_info *fixture_video_info(const video_t *v)
{
	(void)v;
	return &voi;
}
static bool fixture_ovi(struct obs_video_info *v)
{
	memset(v, 0, sizeof(*v));
	v->fps_num = 15;
	v->fps_den = 1;
	v->base_width = v->output_width = 320;
	v->base_height = v->output_height = 180;
	v->output_format = VIDEO_FORMAT_I420;
	return true;
}
static bool fixture_aoi(struct obs_audio_info *a)
{
	a->samples_per_sec = 48000;
	a->speakers = SPEAKERS_STEREO;
	return true;
}
static obs_data_t *fixture_encoder_settings(const obs_encoder_t *e)
{
	(void)e;
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "bitrate", 128);
	obs_data_set_int(s, "keyint_sec", 1);
	return s;
}
static size_t fixture_frame_size(const obs_encoder_t *e)
{
	(void)e;
	return 1024;
}
static bool fixture_extra(const obs_encoder_t *e, uint8_t **data, size_t *size)
{
	(void)e;
	if (starting && starting != healthy_stream && strstr(mode, "late-") && !late_av_available()) {
		os_atomic_inc_long(&invalid_metadata);
		return false;
	}
	if (metadata_gate && e == video_encoder) {
		metadata_gate = false;
		metadata_checkpoint();
	}
	if (fail_extra)
		return false;
	*data = NULL;
	*size = 0;
	return true;
}
struct io_record {
	AVIOContext *io;
	struct ffmpeg_output *owner;
	bool closed;
};
static struct io_record ios[32];
static int io_count, io_closed;
struct format_record {
	AVFormatContext *format;
	struct ffmpeg_output *owner;
	bool freed;
};
static struct format_record formats[32];
static int format_count, format_freed;
static int fixture_open(AVIOContext **io, const char *url, int flags, const AVIOInterruptCB *cb, AVDictionary **options)
{
	(void)url;
	(void)cb;
	(void)options;
	if (starting != healthy_stream && strstr(mode, "open-fail"))
		return AVERROR(EACCES);
	char path[4096];
	snprintf(path, sizeof(path), "%s-io-%d.ts", prefix, io_count + 1);
	FILE *fresh = fopen(path, "wx");
	if (!fresh)
		return AVERROR(EEXIST);
	fclose(fresh);
	if (remove(path))
		return AVERROR(EIO);
	int ret = avio_open(io, path, flags);
	if (ret >= 0)
		ios[io_count++] = (struct io_record){*io, starting, false};
	return ret;
}
static int fixture_close(AVIOContext *io)
{
	if (heavy_gate && gate_avio) {
		heavy_gate = false;
		heavy_checkpoint();
	}
	guarded_metadata_retirement();
	if (!io)
		return avio_close(io);
	for (int i = io_count - 1; i >= 0; i--)
		if (ios[i].io == io && !ios[i].closed) {
			ios[i].closed = true;
			io_closed++;
			return avio_close(io);
		}
	fprintf(stderr, "duplicate AVIO close\n");
	abort();
}
static int fixture_alloc(AVFormatContext **out, const AVOutputFormat *f, const char *name, const char *url)
{
	int ret = avformat_alloc_output_context2(out, f, name, url);
	if (ret >= 0 && *out)
		formats[format_count++] = (struct format_record){*out, starting, false};
	return ret;
}
static void fixture_free_format(AVFormatContext *f)
{
	guarded_metadata_retirement();
	if (!f)
		return;
	for (int i = format_count - 1; i >= 0; i--)
		if (formats[i].format == f && !formats[i].freed) {
			formats[i].freed = true;
			format_freed++;
			avformat_free_context(f);
			return;
		}
	fprintf(stderr, "duplicate AVFormat free\n");
	abort();
}
const char *obs_module_text(const char *s)
{
	return s;
}
#define obs_output_begin_data_capture fixture_begin
#define obs_output_can_begin_data_capture fixture_can
#define obs_output_initialize_encoders fixture_init_encoders
#define obs_output_end_data_capture fixture_end
#define obs_output_signal_stop fixture_signal
#define obs_output_set_last_error fixture_error
#define obs_output_get_service fixture_service
#define obs_service_get_connect_info fixture_connect_info
#define obs_output_get_width fixture_width
#define obs_output_get_height fixture_height
#define obs_output_get_video_encoder fixture_video
#define obs_output_get_audio_encoder fixture_audio
#define obs_encoder_get_codec fixture_codec
#define obs_encoder_video fixture_encoder_video
#define video_output_get_format fixture_format
#define video_output_get_info fixture_video_info
#define obs_get_video_info fixture_ovi
#define obs_get_audio_info fixture_aoi
#define obs_encoder_get_settings fixture_encoder_settings
#define obs_encoder_get_frame_size fixture_frame_size
#define obs_encoder_get_extra_data fixture_extra
#define pthread_mutex_unlock fixture_unlock
#define pthread_mutex_lock fixture_lock
#define avformat_write_header fixture_write_header
#define avcodec_free_context fixture_codec_free
#define pthread_create fixture_create
#define pthread_join fixture_join
#define os_sem_wait fixture_wait
#define avio_open2 fixture_open
#define avio_close fixture_close
#define avformat_alloc_output_context2 fixture_alloc
#define avformat_free_context fixture_free_format
#define av_interleaved_write_frame fixture_interleaved
#include TEST_MPEGTS_SOURCE
#undef av_interleaved_write_frame
#undef pthread_mutex_unlock
#undef pthread_mutex_lock
#undef avformat_write_header
#undef avcodec_free_context
#undef pthread_create
#undef pthread_join
#undef os_sem_wait
#undef avio_close
#undef avformat_free_context
struct tracked {
	pthread_t id;
	void *(*fn)(void *);
	void *arg;
	bool joined;
	volatile bool exited;
};
static struct tracked threads[64];
static int thread_count;
static void *task(void *p)
{
	struct tracked *t = p;
	if (t->fn == start_stop_thread_fn)
		os_event_wait(control_release);
	void *ret = t->fn(t->arg);
	os_atomic_set_bool(&t->exited, true);
	return ret;
}
static struct tracked *tracked(pthread_t t)
{
	for (int i = thread_count - 1; i >= 0; i--)
		if (pthread_equal(threads[i].id, t))
			return &threads[i];
	return NULL;
}
static bool await_writer(struct ffmpeg_output *s)
{
	struct tracked *t = tracked(s->write_thread);
	uint64_t end = os_gettime_ns() + 2000000000ULL;
	while (t && !os_atomic_load_bool(&t->exited) && os_gettime_ns() < end)
		os_sleep_ms(1);
	return t && os_atomic_load_bool(&t->exited);
}
static int fixture_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	if (fn == write_thread && arg != healthy_stream && strstr(mode, "worker-create-fail"))
		return EAGAIN;
	if (thread_count == 64)
		abort();
	struct tracked *row = &threads[thread_count++];
	memset(row, 0, sizeof(*row));
	row->fn = fn;
	row->arg = arg;
	int ret = pthread_create(t, a, task, row);
	if (ret) {
		thread_count--;
		return ret;
	}
	row->id = *t;
	if (fn == write_thread && arg != healthy_stream && strstr(mode, "early-before")) {
		os_atomic_set_long(&wait_error, 1);
		os_sem_post(((struct ffmpeg_output *)arg)->write_sem);
		worker_checkpoint = await_writer(arg);
		context_live_at_checkpoint = ((struct ffmpeg_output *)arg)->ff_data.output != NULL;
	}
	return 0;
}
static int fixture_join(pthread_t t, void **ret)
{
	if (pthread_equal(t, pthread_self())) {
		os_atomic_inc_long(&self_joins);
		return EDEADLK;
	}
	struct tracked *row = tracked(t);
	if (!row || row->joined) {
		os_atomic_inc_long(&bad_joins);
		return EINVAL;
	}
	int rc = pthread_join(t, ret);
	if (!rc)
		row->joined = true;
	return rc;
}
static bool late_av_available(void)
{
	return starting && starting->ff_data.output != NULL;
}
static int fixture_lock(pthread_mutex_t *mutex)
{
#ifdef TEST_CUSTODY
	if (late_callback_running && pthread_equal(pthread_self(), late_callback_owner) &&
	    mutex == &starting->data_mutex) {
		os_atomic_inc_long(&late_lock_attempts);
		os_event_signal(late_attempted);
	}
#endif
#ifdef TEST_CUSTODY
	if (starting && mutex == &starting->data_mutex && os_atomic_load_bool(&metadata_running) &&
	    !pthread_equal(pthread_self(), metadata_owner)) {
		long expected = 0;
		os_atomic_compare_exchange_long(&retirement_kind, &expected, 1);
		os_event_signal(retirement_checkpoint);
	}
#endif
	int rc = pthread_mutex_lock(mutex);
	if (!rc && starting && mutex == &starting->data_mutex)
		own_data_depth++;
#ifdef TEST_CUSTODY
	/* Safe negative: v1 admits this state and dereferences video->codecpar before
  * the injected encoder-extra hook can run. Record that admission and skip
  * only its now-proven-invalid metadata branch, preserving native cleanup. */
	if (!rc && starting && mutex == &starting->data_mutex && strstr(mode, "late-") &&
	    os_atomic_load_long(&starting->capture_start_state) == MPEGTS_COMMITTED && !starting->got_headers &&
	    !starting->ff_data.output) {
		os_atomic_inc_long(&invalid_metadata);
		starting->got_headers = true;
	}
#endif
	return rc;
}
static int fixture_unlock(pthread_mutex_t *mutex)
{
#ifdef TEST_CUSTODY
	/* End the guarded AV-use interval before the actual data unlock. The success
  * callback has no AV accesses after this unlock; function-return timing is not
  * the lifetime barrier. Before/unsafe negatives still wait native completion. */
	if (starting && mutex == &starting->data_mutex && os_atomic_load_bool(&metadata_running) &&
	    pthread_equal(pthread_self(), metadata_owner))
		os_atomic_set_bool(&metadata_running, false);
#endif
	if (starting && mutex == &starting->data_mutex)
		own_data_depth--;
	return pthread_mutex_unlock(mutex);
}
static void metadata_checkpoint(void)
{
	os_event_signal(metadata_entered);
	os_event_wait(metadata_release);
}
static void guarded_metadata_retirement(void)
{
	if (os_atomic_load_bool(&metadata_running) && !pthread_equal(pthread_self(), metadata_owner)) {
		os_atomic_inc_long(&concurrent_av_free);
		long expected = 0;
		os_atomic_compare_exchange_long(&retirement_kind, &expected, 2);
		os_event_signal(retirement_checkpoint);
		if (os_event_timedwait(metadata_finished, 3000))
			abort();
	}
}
static void fixture_codec_free(AVCodecContext **context)
{
	if (context && *context) {
		if (heavy_gate && !gate_avio) {
			heavy_gate = false;
			heavy_checkpoint();
		}
		guarded_metadata_retirement();
		if (late_retirement_gate) {
			late_retirement_gate = false;
			late_callback_running = true;
			if (pthread_create(&late_callback_thread, NULL, late_callback, starting))
				abort();
			if (os_event_timedwait(late_attempted, 3000))
				abort();
		}
	}
	avcodec_free_context(context);
}
static int fixture_write_header(AVFormatContext *format, AVDictionary **options)
{
	int result = avformat_write_header(format, options);
	if (!result && os_atomic_load_bool(&metadata_running))
		os_atomic_set_long(&native_header_completed, 1);
	return result;
}
static void *metadata_worker(void *arg)
{
	struct ffmpeg_output *s = arg;
	metadata_owner = pthread_self();
	os_atomic_set_bool(&metadata_running, true);
	static const uint8_t payload[] = {0, 0, 0, 1, 9, 0x10};
	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO,
					.timebase_den = 1000,
					.data = (uint8_t *)payload,
					.size = sizeof(payload),
					.pts = 1000,
					.dts = 1000,
					.keyframe = true};
	ffmpeg_mpegts_data(s, &packet);
	os_atomic_set_bool(&metadata_running, false);
	os_event_signal(metadata_finished);
	return NULL;
}
static void *late_callback(void *arg)
{
	late_callback_owner = pthread_self();
#ifndef TEST_CUSTODY
	os_event_signal(late_attempted);
#endif
	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO, .timebase_den = 1000};
	os_atomic_inc_long(&late_calls);
	ffmpeg_mpegts_data(arg, &packet);
	return NULL;
}
static void *metadata_observer(void *arg)
{
	struct ffmpeg_output *s = arg;
	if (os_event_timedwait(retirement_checkpoint, 3000))
		abort();
	struct tracked *writer = tracked(s->write_thread);
	observer_writer_exited = writer && os_atomic_load_bool(&writer->exited);
	observer_live_context = s->ff_data.output && s->ff_data.output->pb && os_atomic_load_bool(&metadata_running);
	os_event_signal(metadata_release);
	return NULL;
}
static int fixture_wait(os_sem_t *sem)
{
	int ret = os_sem_wait(sem);
	long expected = 1;
	if (!ret && starting && sem == starting->write_sem &&
	    os_atomic_compare_exchange_long(&wait_error, &expected, 0))
		return EIO;
	return ret;
}
static void fixture_signal(obs_output_t *o, int code)
{
	os_atomic_inc_long(&signals);
	last_code = code;
	if (starting && o == starting->output && strstr(mode, "notify-stop"))
		ffmpeg_mpegts_stop(starting, 0);
}
static bool fixture_begin(obs_output_t *o, uint32_t flags)
{
	(void)flags;
	os_atomic_inc_long(&begin_calls);
	if (!starting || o != starting->output || starting == healthy_stream)
		return true;
	long before = signals;
	if (strstr(mode, "early-")) {
		os_atomic_set_long(&wait_error, 1);
		os_sem_post(starting->write_sem);
		worker_checkpoint = await_writer(starting);
		context_live_at_checkpoint = starting->ff_data.output != NULL;
	}
	if (strstr(mode, "stop-")) {
		starting->start_stop_thread = pthread_self();
		os_atomic_set_bool(&starting->start_stop_thread_active, true);
		ffmpeg_mpegts_stop(starting, 0);
	}
	if (strstr(mode, "metadata-drain")) {
		metadata_gate = true;
		if (pthread_create(&metadata_thread, NULL, metadata_worker, starting) ||
		    pthread_create(&observer_thread, NULL, metadata_observer, starting))
			abort();
		metadata_forwarded = os_event_timedwait(metadata_entered, 3000) == 0;
		if (!metadata_forwarded)
			abort();
	} else if (strstr(mode, "metadata-")) {
		struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO, .timebase_den = 1000};
		ffmpeg_mpegts_data(starting, &packet);
	}
	if (strstr(mode, "header-")) {
		fail_extra = true;
		struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO};
		ffmpeg_mpegts_data(starting, &packet);
		fail_extra = false;
	}
	signals_in_begin = signals - before;
	return begin_result;
}
static const char *dummy_name(void *p)
{
	(void)p;
	return "native MPEGTS owner";
}
static void *dummy_create(obs_data_t *s, obs_output_t *o)
{
	(void)s;
	return o;
}
static void dummy_destroy(void *p)
{
	(void)p;
}
static bool dummy_start(void *p)
{
	(void)p;
	return false;
}
static void dummy_stop(void *p, uint64_t t)
{
	(void)p;
	(void)t;
}
static void dummy_data(void *p, struct encoder_packet *packet)
{
	(void)p;
	(void)packet;
}
static struct obs_output_info dummy = {.id = "private-mpegts-owner",
				       .flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED,
				       .get_name = dummy_name,
				       .create = dummy_create,
				       .destroy = dummy_destroy,
				       .start = dummy_start,
				       .stop = dummy_stop,
				       .encoded_packet = dummy_data};
static FILE *result;
static int checks, failures;
static void check(bool pass, const char *name)
{
	checks++;
	fprintf(result, "%s\t%s\n", pass ? "PASS" : "FAIL", name);
	fflush(result);
	if (!pass)
		failures++;
}
static bool io_is_open(AVIOContext *io)
{
	for (int i = io_count - 1; i >= 0; i--)
		if (ios[i].io == io)
			return !ios[i].closed;
	return false;
}
static void join_other_threads(void)
{
	os_event_signal(control_release);
	for (int i = 0; i < thread_count; i++)
		if (!threads[i].joined && threads[i].fn != write_thread) {
			pthread_join(threads[i].id, NULL);
			threads[i].joined = true;
		}
}
static void retire_fixture(struct ffmpeg_output *s)
{
	os_atomic_set_bool(&s->stopping, true);
	if (s->write_thread_active) {
		os_event_signal(s->stop_event);
		os_sem_post(s->write_sem);
		struct tracked *t = tracked(s->write_thread);
		if (t && !t->joined) {
			pthread_join(s->write_thread, NULL);
			t->joined = true;
		}
		s->write_thread_active = false;
	}
	join_other_threads();
	os_atomic_set_bool(&s->start_stop_thread_active, false);
	os_atomic_set_bool(&s->running, false);
	ffmpeg_mpegts_data_free(s, &s->ff_data);
	pthread_mutex_destroy(&s->write_mutex);
	pthread_mutex_destroy(&s->start_stop_mutex);
#ifdef TEST_CUSTODY
	pthread_mutex_destroy(&s->data_mutex);
#endif
	os_sem_destroy(s->write_sem);
	os_event_destroy(s->stop_event);
	for (size_t i = 0; i < s->packets.num; i++)
		av_packet_free(&s->packets.array[i]);
	da_free(s->packets);
	bfree(s);
}

static int fixture_interleaved(AVFormatContext *f, AVPacket *p)
{
	int result = av_interleaved_write_frame(f, p);
	if (healthy_stream && f == healthy_stream->ff_data.output) {
		os_atomic_set_long(&healthy_write_result, result);
		os_event_signal(healthy_written);
	}
	return result;
}
static void *retire_owner(void *p)
{
	stop(p, true, 0);
	return NULL;
}
static void *late_owner(void *p)
{
	late_callback(p);
	os_event_signal(late_finished);
	return NULL;
}
static void *healthy_owner(void *p)
{
	static uint8_t bytes[] = {0, 0, 0, 1, 9, 0x10};
	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO,
					.timebase_den = 1000,
					.data = bytes,
					.size = sizeof(bytes),
					.pts = 1000,
					.dts = 1000,
					.keyframe = true};
	ffmpeg_mpegts_data(p, &packet);
	os_event_signal(healthy_finished);
	return NULL;
}
int main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	mode = argv[3];
	prefix = argv[2];
	if (strcmp(mode, "slow-codec") && strcmp(mode, "slow-avio"))
		return 2;
	gate_avio = !strcmp(mode, "slow-avio");
	result = fopen(argv[1], "wx");
	if (!result)
		return 3;
	check(obs_startup("en-US", NULL, NULL), "support core startup");
	os_event_t **events[] = {&control_release,       &metadata_entered, &metadata_release, &metadata_finished,
				 &retirement_checkpoint, &late_attempted,   &heavy_entered,    &heavy_release,
				 &late_finished,         &healthy_finished, &healthy_written};
	for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++)
		if (os_event_init(events[i], OS_EVENT_TYPE_MANUAL))
			return 4;
	obs_register_output(&dummy);
	bool registered = false;
	const char *id;
	for (size_t i = 0; obs_enum_output_types(i, &id); i++)
		if (!strcmp(id, dummy.id))
			registered = true;
	check(registered, "output registration preflight");
	if (!registered)
		return 4;
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "muxer_settings", "");
	obs_output_t *ho = obs_output_create(dummy.id, "healthy", settings, NULL),
		     *fo = obs_output_create(dummy.id, "retiring", settings, NULL);
	check(ho && fo, "two real core output objects preflight");
	if (!ho || !fo)
		return 4;
	healthy_stream = ffmpeg_mpegts_create(settings, ho);
	starting = healthy_stream;
	begin_result = true;
	check(healthy_stream != NULL, "native healthy create");
	if (!healthy_stream)
		return 4;
	check(start(healthy_stream), "healthy worker commits");
	AVFormatContext *hf = healthy_stream->ff_data.output;
	AVIOContext *hio = hf ? hf->pb : NULL;
	check(active(healthy_stream) && hio, "healthy owns native AVIO");
	struct ffmpeg_output *s = ffmpeg_mpegts_create(settings, fo);
	starting = s;
	check(s != NULL, "native target create");
	if (!s)
		return 4;
	check(start(s), "target worker commits");
	static uint8_t bytes[] = {0, 0, 0, 1, 9, 0x10};
	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO,
					.timebase_den = 1000,
					.data = bytes,
					.size = sizeof(bytes),
					.pts = 1000,
					.dts = 1000,
					.keyframe = true};
	ffmpeg_mpegts_data(s, &packet);
	check(s->got_headers && s->ff_data.initialized, "target native MPEGTS header prepared");
	int closed = io_closed, freed = format_freed;
	long sg = signals;
	heavy_gate = true;
	pthread_t retire, late, consumer;
	if (pthread_create(&retire, NULL, retire_owner, s))
		return 4;
	bool entered = os_event_timedwait(heavy_entered, 3000) == 0;
	check(entered, "actual native codec or AVIO disposal reaches heavy gate");
	if (!entered)
		return 4;
	check(os_atomic_load_long(&s->capture_start_state) == MPEGTS_RETIRING,
	      "packet admission remains closed throughout disposal");
	check(!s->write_thread_active, "writer joined before native disposal");
	check(os_atomic_load_long(&disposal_lock_depth) == 0, "native heavy disposal executor owns no data mutex");
	check(!s->ff_data.output && !s->h && !s->s && !s->has_connected,
	      "live AV and transport fields detached before heavy close");
	check(io_closed == closed && format_freed == freed,
	      "retired native resources stay alive until gated close resumes");
	if (pthread_create(&late, NULL, late_owner, s) ||
	    pthread_create(&consumer, NULL, healthy_owner, healthy_stream))
		return 4;
	bool late_early = os_event_timedwait(late_finished, 150) == 0;
	bool healthy_early = os_event_timedwait(healthy_finished, 150) == 0;
	bool writer_early = os_event_timedwait(healthy_written, 150) == 0;
	check(late_early, "actual late data callback returns before heavy close gate release");
	check(healthy_early, "healthy actual data callback advances while target disposal is blocked");
	check(writer_early && os_atomic_load_long(&healthy_write_result) >= 0,
	      "healthy native writer enters real FFmpeg successfully before gate release");
	check(os_event_try(heavy_release) != 0, "all progress witnesses observed while disposal gate remains closed");
	check(active(healthy_stream) && healthy_stream->ff_data.output == hf && hf->pb == hio && io_is_open(hio),
	      "healthy original AV context and IO remain owned");
	os_event_signal(heavy_release);
	pthread_join(retire, NULL);
	pthread_join(late, NULL);
	pthread_join(consumer, NULL);
	check(late_calls == 1, "late callback completes once after both candidate outcomes");
	check(signals == sg + 1, "target stop emits one terminal");
	check(!s->write_thread_active && !s->ff_data.output && !os_atomic_load_bool(&s->running),
	      "target retirement completed without resurrection");
	check(io_closed == closed + 1 && format_freed == freed + 1, "target native AVIO and format close once");
	check(!self_joins && !bad_joins, "native writer joins stay balanced");
	mode = "cleanup";
	retire_fixture(s);
	starting = healthy_stream;
	stop(healthy_stream, true, 0);
	retire_fixture(healthy_stream);
	obs_output_release(fo);
	obs_output_release(ho);
	obs_data_release(settings);
	obs_shutdown();
	check(io_count == io_closed && format_count == format_freed, "all native resources retired exactly once");
	for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++)
		os_event_destroy(*events[i]);
	fprintf(result, "TOTAL\t%d\tFAILURES\t%d\n", checks, failures);
	fclose(result);
	return failures ? 1 : 0;
}
