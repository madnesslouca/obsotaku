/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <docks/OBSDock.hpp>

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <array>

class QLabel;
class QPushButton;

/* Instant replay clips: one click or hotkey saves the replay buffer and plays
 * the last 10/30/60/90 seconds on top of the program, on an extra output
 * channel of the main canvas, so no scene is modified. The overlay ends by
 * itself when the clip ends, or with the dismiss button/hotkey. */
class ReplayDock : public OBSDock {
	Q_OBJECT

public:
	static constexpr uint32_t OVERLAY_CHANNEL = 10;
	static constexpr std::array<int, 4> LENGTHS{10, 30, 60, 90};

	explicit ReplayDock(QWidget *parent = nullptr);
	~ReplayDock() override;

	void Shutdown();

public slots:
	void RequestClip(int seconds);
	void Dismiss();

private slots:
	void PlaySavedReplay();
	void OnMediaStarted();
	void UpdateStatus();

private:
	static void OnFrontendEvent(enum obs_frontend_event event, void *data);
	static void OnHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void OnMediaSignal(void *data, calldata_t *params);
	static int BufferSeconds();
	void SaveHotkeys();
	void LoadHotkeys();

	QLabel *status = nullptr;
	QPushButton *dismissButton = nullptr;
	QPushButton *startButton = nullptr;
	std::array<obs_hotkey_id, 4> clipHotkeys{OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID,
						 OBS_INVALID_HOTKEY_ID};
	obs_hotkey_id dismissHotkey = OBS_INVALID_HOTKEY_ID;
	OBSSceneAutoRelease overlayScene;
	OBSSourceAutoRelease media;
	int pendingSeconds = 0;
	int playingSeconds = 0;
	bool shutDown = false;
};
