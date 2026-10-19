/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QObject>

#include <cstddef>

/* VOD-safe audio and per-scene audio.
 *
 * "Off the VOD" removes a source from the audio track that multistream
 * destinations send as their VOD track (Twitch's second track by default),
 * so it is heard live but not in the saved VOD.
 *
 * Per-scene audio stores a source's volume and mute state inside a scene and
 * applies it whenever that scene becomes the program scene. */
class SceneAudio : public QObject {
	Q_OBJECT

public:
	explicit SceneAudio(QObject *parent = nullptr);
	~SceneAudio() override;

	/* Zero-based track used as the VOD track by the configured channels. */
	static size_t VodTrackIndex();
	static bool IsOffVod(obs_source_t *source);
	static void SetOffVod(obs_source_t *source, bool offVod);

	static bool HasSceneAudio(obs_source_t *scene, obs_source_t *source);
	static void SaveSceneAudio(obs_source_t *scene, obs_source_t *source);
	static void ForgetSceneAudio(obs_source_t *scene, obs_source_t *source);
	static void ApplySceneAudio(obs_source_t *scene);

private:
	static void OnFrontendEvent(enum obs_frontend_event event, void *data);
};
