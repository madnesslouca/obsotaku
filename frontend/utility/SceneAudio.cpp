/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SceneAudio.hpp"

#include "MultistreamChannelStore.hpp"

#include "moc_SceneAudio.cpp"

namespace {
constexpr const char *SCENE_AUDIO_KEY = "otaku_scene_audio";
constexpr size_t MAX_TRACKS = 6;
} // namespace

SceneAudio::SceneAudio(QObject *parent) : QObject(parent)
{
	obs_frontend_add_event_callback(OnFrontendEvent, this);
}

SceneAudio::~SceneAudio()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, this);
}

void SceneAudio::OnFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED)
		return;
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	if (scene)
		ApplySceneAudio(scene);
}

size_t SceneAudio::VodTrackIndex()
{
	for (const auto &channel : MultistreamChannelStore::Load()) {
		if (channel.enabled && channel.vodTrackEnabled && channel.vodTrackIndex < MAX_TRACKS)
			return channel.vodTrackIndex;
	}
	/* Twitch's VOD track convention: track 2. */
	return 1;
}

bool SceneAudio::IsOffVod(obs_source_t *source)
{
	if (!source)
		return false;
	return (obs_source_get_audio_mixers(source) & (1u << VodTrackIndex())) == 0;
}

void SceneAudio::SetOffVod(obs_source_t *source, bool offVod)
{
	if (!source)
		return;
	const uint32_t bit = 1u << VodTrackIndex();
	uint32_t mixers = obs_source_get_audio_mixers(source);
	mixers = offVod ? (mixers & ~bit) : (mixers | bit);
	obs_source_set_audio_mixers(source, mixers);
}

bool SceneAudio::HasSceneAudio(obs_source_t *scene, obs_source_t *source)
{
	if (!scene || !source)
		return false;
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	OBSDataAutoRelease all = obs_data_get_obj(priv, SCENE_AUDIO_KEY);
	return all && obs_data_has_user_value(all, obs_source_get_uuid(source));
}

void SceneAudio::SaveSceneAudio(obs_source_t *scene, obs_source_t *source)
{
	if (!scene || !source)
		return;
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	OBSDataAutoRelease all = obs_data_get_obj(priv, SCENE_AUDIO_KEY);
	if (!all) {
		all = obs_data_create();
		obs_data_set_obj(priv, SCENE_AUDIO_KEY, all);
	}
	OBSDataAutoRelease entry = obs_data_create();
	obs_data_set_double(entry, "volume", obs_source_get_volume(source));
	obs_data_set_bool(entry, "muted", obs_source_muted(source));
	obs_data_set_obj(all, obs_source_get_uuid(source), entry);
}

void SceneAudio::ForgetSceneAudio(obs_source_t *scene, obs_source_t *source)
{
	if (!scene || !source)
		return;
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	OBSDataAutoRelease all = obs_data_get_obj(priv, SCENE_AUDIO_KEY);
	if (all)
		obs_data_erase(all, obs_source_get_uuid(source));
}

void SceneAudio::ApplySceneAudio(obs_source_t *scene)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	OBSDataAutoRelease all = obs_data_get_obj(priv, SCENE_AUDIO_KEY);
	if (!all)
		return;
	for (obs_data_item_t *item = obs_data_first(all); item; obs_data_item_next(&item)) {
		const char *uuid = obs_data_item_get_name(item);
		OBSDataAutoRelease entry = obs_data_item_get_obj(item);
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid);
		if (!source || !entry)
			continue;
		obs_source_set_volume(source, static_cast<float>(obs_data_get_double(entry, "volume")));
		obs_source_set_muted(source, obs_data_get_bool(entry, "muted"));
	}
}
