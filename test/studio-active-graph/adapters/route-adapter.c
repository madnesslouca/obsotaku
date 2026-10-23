/* Private test-core adapter only. Never compile into the installed product.
 * Supplies a graphics-free video-view route and controlled render locks;
 * does not modify timestamps, PCM, buffering or the audio mixing algorithm. */
#include "obs-internal.h"
#include "obs-scene.h"
#include <stdlib.h>

#ifdef STUDIO_TEST_AUDIO_REGISTRY
/* Keep an obsolete reader alive across publication; releasing it queues the
 * snapshot for the non-audio collector, exactly as the real callback does. */
EXPORT void *studio_roots_hold_snapshot(void)
{
	return obs_audio_roots_acquire();
}
EXPORT void studio_roots_drop_snapshot(void *snapshot)
{
	obs_audio_roots_release(snapshot);
}
#endif

/* Group transforms need main-canvas dimensions even in an audio-only host.
 * This unregistered geometry stub never creates a graphics/video pipeline. */
EXPORT void studio_roots_geometry(bool enabled)
{
	if (enabled) {
		struct obs_core_video_mix *mix = calloc(1, sizeof(*mix));
		mix->ovi.base_width = mix->ovi.output_width = 640;
		mix->ovi.base_height = mix->ovi.output_height = 360;
		obs->data.main_canvas->mix = mix;
	} else {
		free(obs->data.main_canvas->mix);
		obs->data.main_canvas->mix = NULL;
	}
}

EXPORT obs_view_t *studio_roots_view(obs_canvas_t *canvas)
{
	return &canvas->view;
}

EXPORT void *studio_roots_attach(obs_canvas_t *canvas, bool mix_audio)
{
	struct obs_core_video_mix *mix = calloc(1, sizeof(*mix));
	if (!mix)
		return NULL;
	mix->view = &canvas->view;
	mix->mix_audio = mix_audio;
	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	pthread_mutex_unlock(&obs->video.mixes_mutex);
#ifdef STUDIO_TEST_AUDIO_REGISTRY
	/* Registry attach supplied by the same production implementation as
	 * obs_view_add2/canvas mix creation, selected only in the after fixture. */
	STUDIO_TEST_AUDIO_REGISTRY_ATTACH(&canvas->view, mix_audio);
	STUDIO_TEST_AUDIO_REGISTRY_COLLECT();
#endif
	return mix;
}

/* Simulate an encoder's additional-resolution mix of the SAME logical view.
 * This route never registers a second audio view in the production registry. */
EXPORT void *studio_roots_shadow(void *opaque)
{
	struct obs_core_video_mix *original = opaque;
	struct obs_core_video_mix *mix = calloc(1, sizeof(*mix));
	if (!mix)
		return NULL;
	mix->view = original->view;
	mix->encoder_only_mix = true;
	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	pthread_mutex_unlock(&obs->video.mixes_mutex);
	return mix;
}

EXPORT void studio_roots_detach(void *opaque)
{
	struct obs_core_video_mix *mix = opaque;
	if (!mix || !obs)
		return;
#ifdef STUDIO_TEST_AUDIO_REGISTRY
	if (mix->view && !mix->encoder_only_mix)
		STUDIO_TEST_AUDIO_REGISTRY_DETACH(mix->view);
#endif
	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0; i < obs->video.mixes.num; i++) {
		if (obs->video.mixes.array[i] == mix) {
			da_erase(obs->video.mixes, i);
			break;
		}
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
	free(mix);
#ifdef STUDIO_TEST_AUDIO_REGISTRY
	STUDIO_TEST_AUDIO_REGISTRY_COLLECT();
#endif
}

EXPORT void studio_roots_lock(void *owner, int kind)
{
	struct obs_view *view = owner;
	struct obs_scene *scene = owner;
	pthread_mutex_lock(kind >= 2 ? &scene->video_mutex : kind == 0 ? &obs->video.mixes_mutex : &view->channels_mutex);
}
EXPORT void studio_roots_unlock(void *owner, int kind)
{
	struct obs_view *view = owner;
	struct obs_scene *scene = owner;
	pthread_mutex_unlock(kind >= 2 ? &scene->video_mutex : kind == 0 ? &obs->video.mixes_mutex : &view->channels_mutex);
}
