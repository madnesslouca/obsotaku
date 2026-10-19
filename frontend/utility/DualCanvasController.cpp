/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "DualCanvasController.hpp"

#include <util/util.hpp>

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <vector>

#include "moc_DualCanvasController.cpp"

using namespace std;

namespace {
constexpr const char *LINK_KEY = "otaku_main_scene_uuid";
constexpr const char *MIRROR_KEY = "otaku_mirror";
constexpr const char *NAME_SUFFIX = " · 9:16";

string LinkedName(obs_source_t *mainScene)
{
	const char *name = obs_source_get_name(mainScene);
	return string(name ? name : "Scene") + NAME_SUFFIX;
}

vector<OBSSource> CanvasScenes(obs_canvas_t *canvas)
{
	vector<OBSSource> scenes;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *source) {
			static_cast<vector<OBSSource> *>(param)->emplace_back(source);
			return true;
		},
		&scenes);
	return scenes;
}

string LinkOf(obs_source_t *portraitScene)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(portraitScene);
	const char *uuid = obs_data_get_string(priv, LINK_KEY);
	return uuid ? uuid : "";
}
} // namespace

const char *DualCanvasController::CanvasName()
{
	return "Vertical 9:16";
}

DualCanvasController::DualCanvasController(QObject *parent) : QObject(parent)
{
	obs_frontend_add_event_callback(OnFrontendEvent, this);
}

DualCanvasController::~DualCanvasController()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, this);
	Detach();
}

void DualCanvasController::OnFrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<DualCanvasController *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		self->Attach();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		self->SyncToProgramScene();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		self->PruneAndRename();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		self->Detach();
		break;
	case OBS_FRONTEND_EVENT_CANVAS_REMOVED:
		if (self->attached && !self->PortraitCanvas()) {
			self->Detach();
		}
		break;
	default:
		break;
	}
}

OBSCanvas DualCanvasController::PortraitCanvas() const
{
	lock_guard lock(canvasMutex);
	if (!weakCanvas) {
		return nullptr;
	}
	OBSCanvas canvas = OBSGetStrongRef(weakCanvas);
	if (canvas && obs_canvas_removed(canvas)) {
		return nullptr;
	}
	return canvas;
}

OBSScene DualCanvasController::CurrentPortraitScene() const
{
	return currentScene;
}

QString DualCanvasController::CurrentMainSceneName() const
{
	OBSSource mainScene = OBSGetStrongRef(currentMainScene);
	return mainScene ? QString::fromUtf8(obs_source_get_name(mainScene)) : QString();
}

void DualCanvasController::Attach()
{
	if (attached) {
		Detach();
	}

	OBSCanvas canvas;
	obs_frontend_canvas_list list = {};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; i++) {
		obs_canvas_t *candidate = list.canvases.array[i];
		const char *name = obs_canvas_get_name(candidate);
		if (name && strcmp(name, CanvasName()) == 0 && !obs_canvas_removed(candidate)) {
			canvas = candidate;
			break;
		}
	}
	obs_frontend_canvas_list_free(&list);

	obs_video_info ovi{};
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "DualCanvas: video is not initialised, vertical canvas not attached");
		return;
	}
	ovi.base_width = PORTRAIT_WIDTH;
	ovi.base_height = PORTRAIT_HEIGHT;
	ovi.output_width = PORTRAIT_WIDTH;
	ovi.output_height = PORTRAIT_HEIGHT;
	if (ovi.fps_den == 0 ||
	    static_cast<uint64_t>(ovi.fps_num) > static_cast<uint64_t>(PORTRAIT_MAX_FPS) * ovi.fps_den) {
		ovi.fps_num = PORTRAIT_MAX_FPS;
		ovi.fps_den = 1;
	}
	ovi.scale_type = OBS_SCALE_BICUBIC;

	if (canvas) {
		/* A canvas loaded from a scene collection has no video mix yet: libobs
		 * only stores its name and flags. Give it the 9:16 video again. */
		obs_video_info current{};
		if (!obs_canvas_get_video_info(canvas, &current) && !obs_canvas_reset_video(canvas, &ovi)) {
			blog(LOG_WARNING, "DualCanvas: could not start video on the vertical canvas");
			return;
		}
	} else {
		/* No MIX_AUDIO: audio already comes from the main canvas, mixing it a
		 * second time would double every source in the recording. */
		OBSCanvasAutoRelease created = obs_frontend_add_canvas(CanvasName(), &ovi, ACTIVATE | SCENE_REF);
		if (!created) {
			blog(LOG_WARNING, "DualCanvas: could not create the vertical canvas");
			return;
		}
		canvas = created.Get();
		blog(LOG_INFO, "DualCanvas: created vertical canvas %ux%u", PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
	}

	{
		lock_guard lock(canvasMutex);
		weakCanvas = OBSGetWeakRef(canvas);
	}
	attached = true;
	PruneAndRename();
	SyncToProgramScene();
}

void DualCanvasController::Detach()
{
	DisconnectSceneSignals();
	currentScene = nullptr;
	currentMainScene = nullptr;
	{
		lock_guard lock(canvasMutex);
		weakCanvas = nullptr;
	}
	const bool wasAttached = attached;
	attached = false;
	if (wasAttached) {
		emit portraitSceneChanged();
	}
}

OBSScene DualCanvasController::FindLinkedScene(obs_source_t *mainScene) const
{
	OBSCanvas canvas = PortraitCanvas();
	if (!canvas || !mainScene) {
		return nullptr;
	}
	const char *uuid = obs_source_get_uuid(mainScene);
	for (auto &source : CanvasScenes(canvas)) {
		if (uuid && LinkOf(source) == uuid) {
			return obs_scene_from_source(source);
		}
	}
	return nullptr;
}

OBSScene DualCanvasController::CreateLinkedScene(obs_source_t *mainScene)
{
	OBSCanvas canvas = PortraitCanvas();
	if (!canvas || !mainScene) {
		return nullptr;
	}

	string name = LinkedName(mainScene);
	for (int i = 2; i < 100; i++) {
		OBSSourceAutoRelease existing = obs_canvas_get_source_by_name(canvas, name.c_str());
		if (!existing) {
			break;
		}
		name = LinkedName(mainScene) + " " + to_string(i);
	}

	OBSSceneAutoRelease scene = obs_canvas_scene_create(canvas, name.c_str());
	if (!scene) {
		return nullptr;
	}
	OBSDataAutoRelease priv = obs_source_get_private_settings(obs_scene_get_source(scene));
	obs_data_set_string(priv, LINK_KEY, obs_source_get_uuid(mainScene));

	/* Start as a mirror of the horizontal scene so a new scene never shows a
	 * black vertical program. */
	obs_sceneitem_t *mirror = obs_scene_add(scene, mainScene);
	if (mirror) {
		OBSDataAutoRelease itemPriv = obs_sceneitem_get_private_settings(mirror);
		obs_data_set_bool(itemPriv, MIRROR_KEY, true);
		FitItemToCanvas(mirror, defaultFit);
	}
	blog(LOG_INFO, "DualCanvas: created vertical scene '%s'", name.c_str());
	return scene.Get();
}

void DualCanvasController::SyncToProgramScene()
{
	if (!attached) {
		return;
	}
	OBSCanvas canvas = PortraitCanvas();
	if (!canvas) {
		return;
	}

	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	if (!mainScene) {
		return;
	}

	OBSScene scene = FindLinkedScene(mainScene);
	if (!scene) {
		scene = CreateLinkedScene(mainScene);
	}
	if (!scene) {
		return;
	}

	currentMainScene = OBSGetWeakRef(mainScene.Get());
	obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
	SetCurrentPortraitScene(scene);
}

void DualCanvasController::PruneAndRename()
{
	OBSCanvas canvas = PortraitCanvas();
	if (!canvas) {
		return;
	}
	for (auto &source : CanvasScenes(canvas)) {
		const string link = LinkOf(source);
		if (link.empty()) {
			continue; /* A scene the user or a plugin made on purpose. */
		}
		OBSSourceAutoRelease mainScene = obs_get_source_by_uuid(link.c_str());
		if (!mainScene || obs_source_removed(mainScene)) {
			if (currentScene && obs_scene_get_source(currentScene) == source.Get()) {
				continue;
			}
			blog(LOG_INFO, "DualCanvas: removing orphaned vertical scene '%s'",
			     obs_source_get_name(source));
			obs_source_remove(source);
			continue;
		}
		const string wanted = LinkedName(mainScene);
		const char *current = obs_source_get_name(source);
		if (current && strncmp(current, wanted.c_str(), wanted.size()) != 0) {
			OBSSourceAutoRelease clash = obs_canvas_get_source_by_name(canvas, wanted.c_str());
			if (!clash) {
				obs_source_set_name(source, wanted.c_str());
			}
		}
	}
}

void DualCanvasController::SetCurrentPortraitScene(obs_scene_t *scene)
{
	if (currentScene.Get() == scene) {
		return;
	}
	DisconnectSceneSignals();
	currentScene = scene;
	ConnectSceneSignals(scene);
	emit portraitSceneChanged();
}

void DualCanvasController::ConnectSceneSignals(obs_scene_t *scene)
{
	if (!scene) {
		return;
	}
	signal_handler_t *handler = obs_source_get_signal_handler(obs_scene_get_source(scene));
	for (const char *name : {"item_add", "item_remove", "reorder", "refresh", "item_visible"}) {
		signal_handler_connect(handler, name, OnSceneItemSignal, this);
	}
	signalsConnected = true;
}

void DualCanvasController::DisconnectSceneSignals()
{
	if (!signalsConnected || !currentScene) {
		return;
	}
	signal_handler_t *handler = obs_source_get_signal_handler(obs_scene_get_source(currentScene));
	for (const char *name : {"item_add", "item_remove", "reorder", "refresh", "item_visible"}) {
		signal_handler_disconnect(handler, name, OnSceneItemSignal, this);
	}
	signalsConnected = false;
}

void DualCanvasController::OnSceneItemSignal(void *data, calldata_t *)
{
	/* libobs may emit from any thread; the dock is repainted on the UI thread. */
	QPointer<DualCanvasController> self(static_cast<DualCanvasController *>(data));
	QMetaObject::invokeMethod(
		self.data(),
		[self]() {
			if (self) {
				emit self->layersChanged();
			}
		},
		Qt::QueuedConnection);
}

bool DualCanvasController::IsMirrorItem(obs_sceneitem_t *item)
{
	if (!item) {
		return false;
	}
	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(item);
	return obs_data_get_bool(priv, MIRROR_KEY);
}

void DualCanvasController::FitItemToCanvas(obs_sceneitem_t *item, MultiStreamPortraitFit fit)
{
	if (!item) {
		return;
	}
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_type(item, fit == MultiStreamPortraitFit::Fill ? OBS_BOUNDS_SCALE_OUTER
										: OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_scale_filter(item, OBS_SCALE_LANCZOS);
	const vec2 bounds{static_cast<float>(PORTRAIT_WIDTH), static_cast<float>(PORTRAIT_HEIGHT)};
	const vec2 position{PORTRAIT_WIDTH / 2.0f, PORTRAIT_HEIGHT / 2.0f};
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_pos(item, &position);
	obs_sceneitem_defer_update_end(item);
}

bool DualCanvasController::AddSource(obs_source_t *source)
{
	if (!currentScene || !source) {
		return false;
	}
	obs_sceneitem_t *item = obs_scene_add(currentScene, source);
	if (!item) {
		return false;
	}
	FitItemToCanvas(item, MultiStreamPortraitFit::Fit);
	return true;
}

void DualCanvasController::SetMirrorFit(MultiStreamPortraitFit fit)
{
	defaultFit = fit;
	if (!currentScene) {
		return;
	}
	obs_scene_enum_items(
		currentScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (IsMirrorItem(item)) {
				FitItemToCanvas(item, *static_cast<MultiStreamPortraitFit *>(param));
			}
			return true;
		},
		&fit);
}

bool DualCanvasController::MirrorFit(MultiStreamPortraitFit &fit) const
{
	if (!currentScene) {
		return false;
	}
	struct Result {
		bool found = false;
		MultiStreamPortraitFit fit = MultiStreamPortraitFit::Fill;
	} result;
	obs_scene_enum_items(
		currentScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (!IsMirrorItem(item)) {
				return true;
			}
			auto *out = static_cast<Result *>(param);
			out->found = true;
			out->fit = obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_SCALE_INNER
					   ? MultiStreamPortraitFit::Fit
					   : MultiStreamPortraitFit::Fill;
			return false;
		},
		&result);
	if (result.found) {
		fit = result.fit;
	}
	return result.found;
}

void DualCanvasController::ResetToMirror()
{
	OBSSource mainScene = OBSGetStrongRef(currentMainScene);
	if (!currentScene || !mainScene) {
		return;
	}
	vector<OBSSceneItem> items;
	obs_scene_enum_items(
		currentScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		},
		&items);
	for (auto &item : items) {
		obs_sceneitem_remove(item);
	}
	obs_sceneitem_t *mirror = obs_scene_add(currentScene, mainScene);
	if (mirror) {
		OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(mirror);
		obs_data_set_bool(priv, MIRROR_KEY, true);
		FitItemToCanvas(mirror, defaultFit);
	}
}
