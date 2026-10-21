#pragma once

#include <obs.hpp>
#include <string>
#include <functional>

// Structural snapshots of one composition. Dependencies are recreated only
// when missing; existing shared inputs and nested scenes are never reloaded.
OBSData CaptureStudioSceneEdit(obs_scene_t *scene);
void ApplyStudioSceneEdit(obs_scene_t *scene, const std::function<void(obs_scene_t *)> &change);
bool StudioSceneEditChanged(obs_data_t *before, obs_data_t *after);
bool RestoreStudioSceneEdit(const std::string &json, int64_t &selectedId);

inline constexpr const char *StudioMirrorFitKey = "studioPortraitMirrorFit";
