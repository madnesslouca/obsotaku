/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "MultiStreamManager.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QObject>

#include <mutex>
#include <string>

/* Dual-canvas scenes: every scene of the main (16:9) canvas owns a linked
 * scene on a persistent 1080x1920 "Vertical" canvas. Switching the program
 * scene switches the vertical canvas to the linked scene, so the horizontal
 * and the vertical layout of a scene always travel together.
 *
 * A new linked scene starts as a mirror of the horizontal scene (the main
 * scene nested and scaled to fill or fit 9:16). The user can then add, hide,
 * reorder or remove layers from the vertical preview dock. The canvas and its
 * scenes are stored in the scene collection by the regular OBS canvas code.
 *
 * Everything here runs on the Qt UI thread except PortraitCanvas(), which is
 * safe to call from any thread. */
class DualCanvasController : public QObject {
	Q_OBJECT

public:
	enum class PortraitTransitionMode { Cut = 0, Fade = 1, FollowMain = 2 };
	static constexpr uint32_t PORTRAIT_WIDTH = 1080;
	static constexpr uint32_t PORTRAIT_HEIGHT = 1920;
	static const char *CanvasName();

	explicit DualCanvasController(QObject *parent = nullptr);
	~DualCanvasController() override;

	/* Finds or creates the vertical canvas of the loaded scene collection and
	 * links it to the current program scene. */
	void Attach();
	/* Drops every reference before OBS tears the scene collection down. */
	void Detach();

	/* Follows the program scene. Called on scene change events. */
	Q_INVOKABLE void SyncToProgramScene();
	/* Removes linked scenes whose horizontal scene was deleted and renames
	 * linked scenes whose horizontal scene was renamed. */
	void PruneAndRename();

	OBSCanvas PortraitCanvas() const;
	OBSScene CurrentPortraitScene() const;
	QString CurrentMainSceneName() const;
	/* Applies to the next program change. The editable root remains a scene;
	 * only the canvas' program channel may contain a private transition. */
	Q_INVOKABLE bool SetPortraitTransition(int mode, int durationMs);
	Q_INVOKABLE int PortraitTransitionModeValue() const;
	Q_INVOKABLE int PortraitTransitionDuration() const;

	/* Layer editing on the current vertical scene. */
	/* Group sources get a recursively independent group tree; normal leaf
	 * sources remain references so physical inputs are never reopened. */
	bool AddSource(obs_source_t *source);
	void SetMirrorFit(MultiStreamPortraitFit fit);
	bool MirrorFit(MultiStreamPortraitFit &fit) const;
	void ResetToMirror();
	static void FitItemToCanvas(obs_sceneitem_t *item, MultiStreamPortraitFit fit);
	static bool IsMirrorItem(obs_sceneitem_t *item);

signals:
	/* The vertical canvas switched to another scene, or was (re)created. */
	void portraitSceneChanged();
	/* Layers of the current vertical scene changed. */
	void layersChanged();
	void portraitTransitionSettingsChanged();

private:
	static void OnFrontendEvent(enum obs_frontend_event event, void *data);
	static void OnVideoReset(void *data, calldata_t *params);
	static void OnSceneItemSignal(void *data, calldata_t *params);
	static void OnPortraitSceneRemoved(void *data, calldata_t *params);
	static void OnMainTransitionStart(void *data, calldata_t *params);
	void SyncToMainScene(obs_source_t *mainScene, bool animate, const char *mainTransitionId);
	void SetProgramScene(obs_scene_t *scene, bool animate, const char *mainTransitionId);
	void ObserveMainTransition();

	OBSScene FindLinkedScene(obs_source_t *mainScene) const;
	OBSScene CreateLinkedScene(obs_source_t *mainScene);
	void SetCurrentPortraitScene(obs_scene_t *scene);
	void ConnectSceneSignals(obs_scene_t *scene);
	void DisconnectSceneSignals();

	mutable std::mutex canvasMutex;
	OBSWeakCanvas weakCanvas;
	OBSScene currentScene;
	OBSWeakSource currentMainScene;
	OBSSource portraitTransition;
	OBSSource observedMainTransition;
	OBSSignal mainTransitionStart;
	bool attached = false;
	bool signalsConnected = false;
	bool frontendReady = false;
};
