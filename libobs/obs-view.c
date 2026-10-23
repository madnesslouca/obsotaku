/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "obs.h"
#include "obs-internal.h"

/* Registry writers may hold a view's channels_mutex or the video mixes_mutex.
 * They serialize allocations using update_mutex; roots_mutex protects only
 * pointer publication/refcounts. Source release/destruction is outside BOTH.
 * Audio takes only roots_mutex and never allocates/frees a registry snapshot. */
static void free_audio_roots(struct obs_audio_roots *roots)
{
	while (roots) {
		struct obs_audio_roots *next = roots->next_retired;
		for (size_t i = 0; i < roots->num; i++)
			obs_source_release(roots->roots[i].source);
		bfree(roots->roots);
		bfree(roots);
		roots = next;
	}
}

/* update_mutex held; retire only. Caller drains after releasing outer locks. */
static void publish_audio_roots(void)
{
	struct obs_core_data *data = &obs->data;
	struct obs_audio_roots *roots = bzalloc(sizeof(*roots));
	roots->refs = 1;
	for (struct obs_view *view = data->audio_active_views; view; view = view->audio_active_next) {
		if (!view->audio_registered)
			continue;
		for (size_t i = 0; i < MAX_CHANNELS; i++)
			if (view->audio_channels[i])
				roots->num++;
	}

	if (roots->num)
		roots->roots = bmalloc(roots->num * sizeof(*roots->roots));
	size_t num = 0;
	for (struct obs_view *view = data->audio_active_views; view; view = view->audio_active_next) {
		if (!view->audio_registered)
			continue;
		for (size_t i = 0; i < MAX_CHANNELS; i++) {
			obs_source_t *source = obs_source_get_ref(view->audio_channels[i]);
			if (source)
				roots->roots[num++] = (struct obs_audio_root){source, view->audio_mix};
		}
	}
	roots->num = num;

	pthread_mutex_lock(&data->audio_roots_mutex);
	struct obs_audio_roots *prev = data->audio_roots;
	data->audio_roots = roots;
	if (prev && --prev->refs == 0) {
		prev->next_retired = data->audio_retired_roots;
		data->audio_retired_roots = prev;
	}
	pthread_mutex_unlock(&data->audio_roots_mutex);
}

struct obs_audio_roots *obs_audio_roots_acquire(void)
{
	struct obs_core_data *data = &obs->data;
	pthread_mutex_lock(&data->audio_roots_mutex);
	struct obs_audio_roots *roots = data->audio_roots;
	if (roots)
		roots->refs++;
	pthread_mutex_unlock(&data->audio_roots_mutex);
	return roots;
}

void obs_audio_roots_release(struct obs_audio_roots *roots)
{
	if (!roots)
		return;
	struct obs_core_data *data = &obs->data;
	pthread_mutex_lock(&data->audio_roots_mutex);
	if (--roots->refs == 0) {
		roots->next_retired = data->audio_retired_roots;
		data->audio_retired_roots = roots;
	}
	pthread_mutex_unlock(&data->audio_roots_mutex);
}

void obs_audio_view_add(struct obs_view *view, bool mix_audio)
{
	struct obs_core_data *data = &obs->data;
	pthread_mutex_lock(&data->audio_roots_update_mutex);
	if (data->audio_roots_shutting_down) {
		pthread_mutex_unlock(&data->audio_roots_update_mutex);
		return;
	}
	if (view->audio_registered && (view->audio_mix || !mix_audio)) {
		pthread_mutex_unlock(&data->audio_roots_update_mutex);
		return;
	}
	if (!view->audio_registered) {
		struct obs_view **link = &data->audio_active_views;
		while (*link)
			link = &(*link)->audio_active_next;
		*link = view;
		view->audio_active_next = NULL;
		view->audio_registered = true;
	}
	view->audio_mix |= mix_audio;
	publish_audio_roots();
	pthread_mutex_unlock(&data->audio_roots_update_mutex);
}

void obs_audio_view_remove(struct obs_view *view)
{
	struct obs_core_data *data = &obs->data;
	pthread_mutex_lock(&data->audio_roots_update_mutex);
	if (!view->audio_registered) {
		pthread_mutex_unlock(&data->audio_roots_update_mutex);
		return;
	}
	struct obs_view **link = &data->audio_active_views;
	while (*link && *link != view)
		link = &(*link)->audio_active_next;
	if (*link)
		*link = view->audio_active_next;
	view->audio_active_next = NULL;
	view->audio_registered = false;
	view->audio_mix = false;
	publish_audio_roots();
	pthread_mutex_unlock(&data->audio_roots_update_mutex);
}

obs_source_t *obs_audio_view_set_source(struct obs_view *view, uint32_t channel, obs_source_t *source)
{
	struct obs_core_data *data = &obs->data;
	pthread_mutex_lock(&data->audio_roots_update_mutex);
	if (data->audio_roots_shutting_down) {
		pthread_mutex_unlock(&data->audio_roots_update_mutex);
		return NULL;
	}
	obs_source_t *prev = view->audio_channels[channel];
	if (prev == source) {
		pthread_mutex_unlock(&data->audio_roots_update_mutex);
		return NULL;
	}
	view->audio_channels[channel] = obs_source_get_ref(source);
	if (view->audio_registered)
		publish_audio_roots();
	pthread_mutex_unlock(&data->audio_roots_update_mutex);
	return prev;
}

void obs_audio_view_free(struct obs_view *view)
{
	struct obs_core_data *data = &obs->data;
	obs_source_t *sources[MAX_CHANNELS];
	pthread_mutex_lock(&data->audio_roots_update_mutex);
	struct obs_view **link = &data->audio_views;
	while (*link && *link != view)
		link = &(*link)->audio_next;
	if (*link)
		*link = view->audio_next;
	view->audio_next = NULL;
	bool registered = view->audio_registered;
	link = &data->audio_active_views;
	while (*link && *link != view)
		link = &(*link)->audio_active_next;
	if (*link)
		*link = view->audio_active_next;
	view->audio_active_next = NULL;
	view->audio_registered = false;
	view->audio_mix = false;
	memcpy(sources, view->audio_channels, sizeof(sources));
	memset(view->audio_channels, 0, sizeof(view->audio_channels));
	if (registered)
		publish_audio_roots();
	pthread_mutex_unlock(&data->audio_roots_update_mutex);
	for (size_t i = 0; i < MAX_CHANNELS; i++)
		obs_source_release(sources[i]);
	obs_audio_roots_collect();
}

void obs_audio_roots_collect(void)
{
	struct obs_core_data *data = &obs->data;
	pthread_mutex_lock(&data->audio_roots_mutex);
	struct obs_audio_roots *retired = data->audio_retired_roots;
	data->audio_retired_roots = NULL;
	pthread_mutex_unlock(&data->audio_roots_mutex);
	free_audio_roots(retired);
}

void obs_audio_roots_free(void)
{
	/* Shutdown only: video/audio are stopped, before plugin/type-data teardown.
	 * Close publication first. Transfer mirror refs while views are protected
	 * by update_mutex, then release them without touching those views again. */
	struct obs_core_data *data = &obs->data;
	if (!data->audio_roots_initialized)
		return;
	pthread_mutex_lock(&data->audio_roots_update_mutex);
	data->audio_roots_shutting_down = true;
	size_t num = 0;
	for (struct obs_view *view = data->audio_views; view; view = view->audio_next)
		for (size_t i = 0; i < MAX_CHANNELS; i++)
			if (view->audio_channels[i])
				num++;
	struct obs_audio_roots *mirrors = num ? bzalloc(sizeof(*mirrors)) : NULL;
	if (mirrors)
		mirrors->roots = bmalloc(num * sizeof(*mirrors->roots));
	struct obs_view *view = data->audio_views;
	while (view) {
		struct obs_view *next = view->audio_next;
		for (size_t i = 0; i < MAX_CHANNELS; i++) {
			if (view->audio_channels[i])
				mirrors->roots[mirrors->num++].source = view->audio_channels[i];
			view->audio_channels[i] = NULL;
		}
		view->audio_registered = false;
		view->audio_mix = false;
		view->audio_next = NULL;
		view->audio_active_next = NULL;
		view = next;
	}
	data->audio_views = NULL;
	data->audio_active_views = NULL;
	pthread_mutex_lock(&data->audio_roots_mutex);
	struct obs_audio_roots *roots = data->audio_roots;
	if (roots) {
		assert(roots->refs == 1);
		roots->next_retired = data->audio_retired_roots;
	} else {
		roots = data->audio_retired_roots;
	}
	data->audio_roots = NULL;
	data->audio_retired_roots = NULL;
	pthread_mutex_unlock(&data->audio_roots_mutex);
	pthread_mutex_unlock(&data->audio_roots_update_mutex);
	if (mirrors) {
		mirrors->next_retired = roots;
		roots = mirrors;
	}
	free_audio_roots(roots);
}

bool obs_view_init(struct obs_view *view, enum view_type type)
{
	if (!view)
		return false;

	pthread_mutex_init_value(&view->channels_mutex);

	if (pthread_mutex_init(&view->channels_mutex, NULL) != 0) {
		blog(LOG_ERROR, "obs_view_init: Failed to create mutex");
		return false;
	}

	view->type = type;
	pthread_mutex_lock(&obs->data.audio_roots_update_mutex);
	if (!obs->data.audio_roots_shutting_down) {
		view->audio_next = obs->data.audio_views;
		obs->data.audio_views = view;
	}
	pthread_mutex_unlock(&obs->data.audio_roots_update_mutex);
	return true;
}

obs_view_t *obs_view_create(void)
{
	struct obs_view *view = bzalloc(sizeof(struct obs_view));

	if (!obs_view_init(view, AUX_VIEW)) {
		bfree(view);
		view = NULL;
	}

	return view;
}

void obs_view_free(struct obs_view *view)
{
	if (!view)
		return;

	obs_audio_view_free(view);

	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		struct obs_source *source = view->channels[i];
		if (source) {
			obs_source_deactivate(source, view->type);
			obs_source_release(source);
		}
	}

	memset(view->channels, 0, sizeof(view->channels));
	pthread_mutex_destroy(&view->channels_mutex);
}

void obs_view_destroy(obs_view_t *view)
{
	if (view) {
		obs_view_free(view);
		bfree(view);
	}
}

obs_source_t *obs_view_get_source(obs_view_t *view, uint32_t channel)
{
	obs_source_t *source;
	assert(channel < MAX_CHANNELS);

	if (!view)
		return NULL;
	if (channel >= MAX_CHANNELS)
		return NULL;

	pthread_mutex_lock(&view->channels_mutex);
	source = obs_source_get_ref(view->channels[channel]);
	pthread_mutex_unlock(&view->channels_mutex);

	return source;
}

void obs_view_set_source(obs_view_t *view, uint32_t channel, obs_source_t *source)
{
	struct obs_source *prev_source;

	assert(channel < MAX_CHANNELS);

	if (!view)
		return;
	if (channel >= MAX_CHANNELS)
		return;

	pthread_mutex_lock(&view->channels_mutex);
	source = obs_source_get_ref(source);
	prev_source = view->channels[channel];
	view->channels[channel] = source;
	obs_source_t *prev_audio_source = obs_audio_view_set_source(view, channel, source);

	pthread_mutex_unlock(&view->channels_mutex);

	if (source)
		obs_source_activate(source, view->type);

	if (prev_source) {
		obs_source_deactivate(prev_source, view->type);
		obs_source_release(prev_source);
	}
	obs_source_release(prev_audio_source);
	obs_audio_roots_collect();
}

void obs_view_render(obs_view_t *view)
{
	if (!view)
		return;

	pthread_mutex_lock(&view->channels_mutex);

	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		struct obs_source *source;

		source = view->channels[i];

		if (source) {
			if (source->removed) {
				obs_source_release(source);
				view->channels[i] = NULL;
			} else {
				obs_source_video_render(source);
			}
		}
	}

	pthread_mutex_unlock(&view->channels_mutex);
}

video_t *obs_view_add(obs_view_t *view)
{
	if (!obs->data.main_canvas->mix)
		return NULL;
	return obs_view_add2(view, &obs->data.main_canvas->mix->ovi);
}

video_t *obs_view_add2(obs_view_t *view, struct obs_video_info *ovi)
{
	if (!view || !ovi)
		return NULL;

	struct obs_core_video_mix *mix = obs_create_video_mix(ovi);
	if (!mix) {
		return NULL;
	}
	mix->view = view;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	obs_audio_view_add(view, mix->mix_audio);
	video_t *video = mix->video;
	pthread_mutex_unlock(&obs->video.mixes_mutex);
	obs_audio_roots_collect();

	return video;
}

void obs_view_remove(obs_view_t *view)
{
	if (!view)
		return;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	obs_audio_view_remove(view);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		if (obs->video.mixes.array[i]->view == view)
			obs->video.mixes.array[i]->view = NULL;
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
	obs_audio_roots_collect();
}

void obs_view_enum_video_info(obs_view_t *view, bool (*enum_proc)(void *, struct obs_video_info *), void *param)
{
	pthread_mutex_lock(&obs->video.mixes_mutex);

	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (mix->view != view)
			continue;
		if (!enum_proc(param, &mix->ovi))
			break;
	}

	pthread_mutex_unlock(&obs->video.mixes_mutex);
}
