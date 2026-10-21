/* Test-only audio view registration. Never link or install into the product.
 * No source buffers, timestamps, sync offsets or mixer algorithm are modified.
 * libobs's public view APIs create graphics mixes; this supplies only the view
 * entry needed by the real audio_callback in a process without GPU/video. */
#include "obs-internal.h"
#include <stdlib.h>

EXPORT void *studio_audio_sync_attach(void)
{
	if (!obs || obs->video.mixes.num)
		return NULL;
	struct obs_core_video_mix *mix = calloc(1, sizeof(*mix));
	if (!mix)
		return NULL;
	mix->view = &obs->data.main_canvas->view;
	mix->mix_audio = true;
	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	pthread_mutex_unlock(&obs->video.mixes_mutex);
	return mix;
}
EXPORT void studio_audio_sync_detach(void *opaque)
{
	struct obs_core_video_mix *mix = opaque;
	if (!mix || !obs)
		return;
	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0; i < obs->video.mixes.num; ++i) {
		if (obs->video.mixes.array[i] == mix) {
			da_erase(obs->video.mixes, i);
			break;
		}
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
	free(mix);
}
