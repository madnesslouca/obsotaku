/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "DualCanvasController.hpp"
#include "StudioSceneEdit.hpp"
#include <OBSApp.hpp>

#include <util/util.hpp>

#include <QMetaObject>
#include <QPointer>
#include <QUuid>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "moc_DualCanvasController.cpp"

using namespace std;

namespace {
constexpr const char *LINK_KEY = "otaku_main_scene_uuid";
constexpr const char *MIRROR_KEY = "otaku_mirror";
constexpr const char *NAME_SUFFIX = " · 9:16";
constexpr const char *TRANSITION_MODE_KEY = "StudioPortraitTransition";
constexpr const char *TRANSITION_DURATION_KEY = "StudioPortraitTransitionDuration";

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

bool EligiblePortraitScene(obs_scene_t *scene, obs_canvas_t *canvas)
{
	auto *source = obs_scene_get_source(scene);
	if (!source || !canvas || obs_source_removed(source)) {
		return false;
	}
	OBSCanvasAutoRelease owner = obs_source_get_canvas(source);
	return owner == canvas;
}

struct PortraitGroupClone {
	OBSSource original;
	OBSData definition;
	string uuid;
	string name;
};

/* Clone only composition-owned group sources. Ordinary leaves, including
 * cameras and nested normal scenes, remain references. Native serialization
 * preserves transforms, relative coordinates, crop, blending and transitions. */
class PortraitGroupClonePlan {
public:
	explicit PortraitGroupClonePlan(obs_canvas_t *canvas) : canvas(canvas) {}
	~PortraitGroupClonePlan()
	{
		if (!committed) {
			// Parents first, then their newly created dependencies. Never remove
			// an original group, filter or shared capture source on rollback.
			for (auto it = created.rbegin(); it != created.rend(); ++it) {
				obs_source_remove(*it);
			}
		}
	}
	bool Prepare(obs_source_t *source, unsigned depth = 0)
	{
		if (!source || obs_source_removed(source) || !obs_source_is_group(source) || depth > 64) {
			return false;
		}
		const string oldUuid = obs_source_get_uuid(source);
		if (indices.count(oldUuid)) {
			return true;
		}
		if (!visiting.insert(oldUuid).second) {
			return false;
		}
		OBSDataAutoRelease saved = obs_save_source(source);
		OBSDataAutoRelease frozen = saved ? obs_data_create_from_json(obs_data_get_json(saved)) : nullptr;
		if (!frozen) {
			return false;
		}
		OBSDataAutoRelease settings = obs_data_get_obj(frozen, "settings");
		OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
		for (size_t i = 0; i < obs_data_array_count(items); ++i) {
			OBSDataAutoRelease item = obs_data_array_item(items, i);
			if (obs_data_get_bool(item, "group_item_backup")) {
				continue;
			}
			OBSSourceAutoRelease child = obs_get_source_by_uuid(obs_data_get_string(item, "source_uuid"));
			if (!child || obs_source_removed(child)) {
				return false;
			}
			if (obs_source_is_group(child) && !Prepare(child, depth + 1)) {
				return false;
			}
			leaves.emplace_back(child.Get());
		}
		visiting.erase(oldUuid);
		PortraitGroupClone entry;
		entry.original = source;
		entry.definition = frozen.Get();
		entry.uuid = FreshUuid();
		const string base = string(obs_source_get_name(source)) + " · 9:16";
		entry.name = base;
		for (unsigned n = 2; NameExists(entry.name); ++n) {
			entry.name = base + " " + to_string(n);
		}
		reservedNames.insert(entry.name);
		indices[oldUuid] = groups.size();
		groups.emplace_back(std::move(entry));
		return true;
	}
	OBSSource Create(obs_source_t *original)
	{
		for (auto &entry : groups) {
			OBSData definition = entry.definition;
			obs_data_set_string(definition, "uuid", entry.uuid.c_str());
			obs_data_set_string(definition, "name", entry.name.c_str());
			obs_data_set_string(definition, "canvas_uuid", obs_canvas_get_uuid(canvas));
			// A new composition must not reuse the original group's shortcuts.
			// This changes the frozen clone definition, never the original source.
			obs_data_erase(definition, "hotkeys");
			RenewFilterUuids(definition);
			OBSDataAutoRelease settings = obs_data_get_obj(definition, "settings");
			OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
			for (size_t i = 0; i < obs_data_array_count(items); ++i) {
				OBSDataAutoRelease item = obs_data_array_item(items, i);
				// Relative values refer to the old canvas' dimensions. Force the
				// native loader's absolute-pixel fallback when moving to 9:16;
				// it computes fresh relative coordinates and scale_ref there.
				for (const char *key : {"pos_rel", "scale_rel", "scale_ref", "bounds_rel"}) {
					obs_data_erase(item, key);
				}
				const auto found = indices.find(obs_data_get_string(item, "source_uuid"));
				if (found == indices.end()) {
					continue;
				}
				const auto &target = groups[found->second];
				// Includes legacy backup entries emitted by scene_save_item.
				obs_data_set_string(item, "source_uuid", target.uuid.c_str());
				obs_data_set_string(item, "name", target.name.c_str());
			}
			OBSSourceAutoRelease copy = obs_load_source(definition);
			if (!copy) {
				return nullptr;
			}
			created.emplace_back(copy.Get());
			OBSCanvasAutoRelease assignedCanvas = obs_source_get_canvas(copy);
			if (!obs_group_from_source(copy) || obs_source_removed(copy) || assignedCanvas != canvas) {
				return nullptr;
			}
			obs_source_load2(copy); // Only this newly-created group and its cloned filters.
			if (!Matches(copy, items)) {
				return nullptr;
			}
		}
		const auto found = indices.find(obs_source_get_uuid(original));
		if (found == indices.end() || found->second >= created.size()) {
			return nullptr;
		}
		return created[found->second];
	}
	void Commit() { committed = true; }

private:
	static string FreshUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
	bool NameExists(const string &name) const
	{
		if (reservedNames.count(name)) {
			return true;
		}
		OBSSourceAutoRelease existing = obs_get_source_by_name(name.c_str());
		OBSSourceAutoRelease inCanvas = obs_canvas_get_source_by_name(canvas, name.c_str());
		return existing || inCanvas;
	}
	static void RenewFilterUuids(obs_data_t *definition)
	{
		OBSDataArrayAutoRelease filters = obs_data_get_array(definition, "filters");
		for (size_t i = 0; i < obs_data_array_count(filters); ++i) {
			OBSDataAutoRelease filter = obs_data_array_item(filters, i);
			obs_data_set_string(filter, "uuid", FreshUuid().c_str());
			obs_data_erase(filter, "hotkeys");
			RenewFilterUuids(filter);
		}
	}
	static bool Matches(obs_source_t *copy, obs_data_array_t *items)
	{
		obs_scene_t *scene = obs_group_from_source(copy);
		size_t expected = 0, actual = 0;
		for (size_t i = 0; i < obs_data_array_count(items); ++i) {
			OBSDataAutoRelease saved = obs_data_array_item(items, i);
			if (obs_data_get_bool(saved, "group_item_backup")) {
				continue;
			}
			++expected;
			auto *item = obs_scene_find_sceneitem_by_id(scene, obs_data_get_int(saved, "id"));
			if (!item || string(obs_source_get_uuid(obs_sceneitem_get_source(item))) !=
					     obs_data_get_string(saved, "source_uuid")) {
				return false;
			}
		}
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *, void *data) {
				++*static_cast<size_t *>(data);
				return true;
			},
			&actual);
		return expected == actual;
	}
	OBSCanvas canvas;
	vector<PortraitGroupClone> groups;
	map<string, size_t> indices;
	set<string> visiting, reservedNames;
	vector<OBSSource> leaves, created;
	bool committed = false;
};
} // namespace

const char *DualCanvasController::CanvasName()
{
	return "Vertical 9:16";
}

DualCanvasController::DualCanvasController(QObject *parent) : QObject(parent)
{
	setObjectName(QStringLiteral("studioDualCanvasController"));
	obs_frontend_add_event_callback(OnFrontendEvent, this);
	signal_handler_connect(obs_get_signal_handler(), "video_reset", OnVideoReset, this);
}

DualCanvasController::~DualCanvasController()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, this);
	signal_handler_disconnect(obs_get_signal_handler(), "video_reset", OnVideoReset, this);
	Detach();
}

void DualCanvasController::OnFrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<DualCanvasController *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		self->frontendReady = true;
		self->Attach();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		self->Attach();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		self->SyncToProgramScene();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
		self->ObserveMainTransition();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		/* T-bar cancellation restores programScene after emitting this event.
		 * Reconcile on the next UI turn instead of keeping its abandoned target. */
		QMetaObject::invokeMethod(self, [self]() { self->SyncToProgramScene(); }, Qt::QueuedConnection);
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		self->PruneAndRename();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		self->frontendReady = false;
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

void DualCanvasController::OnVideoReset(void *data, calldata_t *)
{
	auto *self = static_cast<DualCanvasController *>(data);
	/* ResetVideo is still rebuilding the main mix when this signal fires. */
	QMetaObject::invokeMethod(self, [self]() { self->Attach(); }, Qt::QueuedConnection);
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
	return EligiblePortraitScene(currentScene, PortraitCanvas()) ? currentScene : OBSScene{};
}

QString DualCanvasController::CurrentMainSceneName() const
{
	OBSSource mainScene = OBSGetStrongRef(currentMainScene);
	return mainScene ? QString::fromUtf8(obs_source_get_name(mainScene)) : QString();
}

void DualCanvasController::Attach()
{
	// Video resets also happen before the collection is loaded. Creating a
	// canvas then steals the persisted name and orphans the restored layouts.
	if (!frontendReady) {
		return;
	}
	/* Reconfiguration is only safe with all encoders and the virtual camera
	 * stopped. Unrelated General settings may be applied during a live. */
	if (obs_video_active() || obs_frontend_virtualcam_active()) {
		return;
	}
	config_t *config = App()->GetUserConfig();
	const bool enabled = !config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
			     config_get_bool(config, "BasicWindow", "StudioMultiCanvas");
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
	if (!enabled) {
		if (canvas) {
			obs_canvas_set_channel(canvas, 0, nullptr);
		}
		Detach();
		return;
	}

	obs_video_info ovi{};
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "DualCanvas: video is not initialised, vertical canvas not attached");
		return;
	}
	ovi.base_width = PORTRAIT_WIDTH;
	ovi.base_height = PORTRAIT_HEIGHT;
	ovi.output_width = PORTRAIT_WIDTH;
	ovi.output_height = PORTRAIT_HEIGHT;
	/* Canvas rendering shares the main clock. A different encoder timebase
	 * produces A/V drift and long stalls when an output reconnects. */
	ovi.scale_type = OBS_SCALE_BICUBIC;

	if (canvas) {
		/* A canvas loaded from a scene collection has no video mix yet: libobs
		 * only stores its name and flags. Give it the 9:16 video again. */
		obs_video_info current{};
		const bool needsReset =
			!obs_canvas_get_video_info(canvas, &current) || current.fps_num != ovi.fps_num ||
			current.fps_den != ovi.fps_den || current.base_width != ovi.base_width ||
			current.base_height != ovi.base_height || current.output_width != ovi.output_width ||
			current.output_height != ovi.output_height || current.output_format != ovi.output_format ||
			current.colorspace != ovi.colorspace || current.range != ovi.range;
		if (needsReset && !obs_canvas_reset_video(canvas, &ovi)) {
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
	ObserveMainTransition();
	PruneAndRename();
	SyncToProgramScene();
}

void DualCanvasController::ObserveMainTransition()
{
	mainTransitionStart.Disconnect();
	observedMainTransition = nullptr;
	if (!attached) {
		return;
	}
	OBSSourceAutoRelease transition = obs_get_output_source(0);
	if (!transition || obs_source_get_type(transition) != OBS_SOURCE_TYPE_TRANSITION) {
		return;
	}
	observedMainTransition = transition.Get();
	// Main transitions are private sources: their global events are omitted.
	mainTransitionStart.Connect(obs_source_get_signal_handler(transition), "transition_start",
				    OnMainTransitionStart, this);
}

void DualCanvasController::Detach()
{
	mainTransitionStart.Disconnect();
	observedMainTransition = nullptr;
	DisconnectSceneSignals();
	if (portraitTransition) {
		OBSCanvas canvas = PortraitCanvas();
		OBSSourceAutoRelease channel = canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
		if (channel == portraitTransition) {
			obs_canvas_set_channel(canvas, 0, nullptr);
		}
		obs_transition_clear(portraitTransition);
		portraitTransition = nullptr;
	}
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
		obs_scene_t *scene = obs_scene_from_source(source);
		if (EligiblePortraitScene(scene, canvas) && uuid && LinkOf(source) == uuid) {
			return scene;
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
		FitItemToCanvas(mirror, MultiStreamPortraitFit::Fill);
	}
	blog(LOG_INFO, "DualCanvas: created vertical scene '%s'", name.c_str());
	return scene.Get();
}

void DualCanvasController::SyncToProgramScene()
{
	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	SyncToMainScene(mainScene, true, nullptr);
}

void DualCanvasController::SyncToMainScene(obs_source_t *mainScene, bool animate, const char *mainTransitionId)
{
	OBSCanvas canvas = PortraitCanvas();
	if (!attached || !canvas || !mainScene || obs_source_removed(mainScene) || !obs_scene_from_source(mainScene)) {
		return;
	}
	OBSSource previousMain = OBSGetStrongRef(currentMainScene);
	const bool currentEligible = EligiblePortraitScene(currentScene, canvas);
	if (previousMain == mainScene && currentEligible) {
		return;
	}
	const bool invalidated = currentScene && !currentEligible;
	if (invalidated) {
		// A retained scene pointer is not proof that its source is still part
		// of this canvas. Drop stale identity before finding its replacement;
		// in particular, never fade from a removed source or skip same-Main sync.
		DisconnectSceneSignals();
		currentScene = nullptr;
		currentMainScene = nullptr;
	}

	OBSScene scene = FindLinkedScene(mainScene);
	if (!scene) {
		scene = CreateLinkedScene(mainScene);
	}
	if (!EligiblePortraitScene(scene, canvas)) {
		if (invalidated) {
			obs_canvas_set_channel(canvas, 0, nullptr);
			if (portraitTransition) {
				obs_transition_clear(portraitTransition);
			}
			emit portraitSceneChanged();
		}
		return;
	}
	if (!attached || PortraitCanvas() != canvas) {
		return;
	}

	SetProgramScene(scene, animate && !!currentScene, mainTransitionId);
	currentMainScene = OBSGetWeakRef(mainScene);
	SetCurrentPortraitScene(scene);
}

int DualCanvasController::PortraitTransitionModeValue() const
{
	const int mode = static_cast<int>(config_get_int(App()->GetUserConfig(), "BasicWindow", TRANSITION_MODE_KEY));
	return mode >= 0 && mode <= 2 ? mode : 0;
}

int DualCanvasController::PortraitTransitionDuration() const
{
	config_t *config = App()->GetUserConfig();
	if (!config_has_user_value(config, "BasicWindow", TRANSITION_DURATION_KEY)) {
		return 300;
	}
	return std::clamp(static_cast<int>(config_get_int(config, "BasicWindow", TRANSITION_DURATION_KEY)), 50, 20000);
}

bool DualCanvasController::SetPortraitTransition(int mode, int durationMs)
{
	if (mode < 0 || mode > 2 || durationMs < 50 || durationMs > 20000) {
		return false;
	}
	config_t *config = App()->GetUserConfig();
	const bool hadMode = config_has_user_value(config, "BasicWindow", TRANSITION_MODE_KEY);
	const bool hadDuration = config_has_user_value(config, "BasicWindow", TRANSITION_DURATION_KEY);
	const int oldMode = PortraitTransitionModeValue();
	const int oldDuration = PortraitTransitionDuration();
	config_set_int(config, "BasicWindow", TRANSITION_MODE_KEY, mode);
	config_set_int(config, "BasicWindow", TRANSITION_DURATION_KEY, durationMs);
	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		if (hadMode) {
			config_set_int(config, "BasicWindow", TRANSITION_MODE_KEY, oldMode);
		} else {
			config_remove_value(config, "BasicWindow", TRANSITION_MODE_KEY);
		}
		if (hadDuration) {
			config_set_int(config, "BasicWindow", TRANSITION_DURATION_KEY, oldDuration);
		} else {
			config_remove_value(config, "BasicWindow", TRANSITION_DURATION_KEY);
		}
		return false;
	}
	emit portraitTransitionSettingsChanged();
	return true;
}

void DualCanvasController::OnMainTransitionStart(void *data, calldata_t *params)
{
	OBSSource source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
	OBSSourceAutoRelease mainProgram = obs_get_output_source(0);
	if (!source || source != mainProgram) {
		return; // Ignore item and Portrait transitions.
	}
	OBSSourceAutoRelease destination = obs_transition_get_source(source, OBS_TRANSITION_SOURCE_B);
	if (!destination || !obs_scene_from_source(destination)) {
		return;
	}
	const string id = obs_source_get_unversioned_id(source);
	QPointer<DualCanvasController> self(static_cast<DualCanvasController *>(data));
	OBSSource target = destination.Get();
	QMetaObject::invokeMethod(
		self.data(),
		[self, target, id]() {
			if (!self || !self->attached) {
				return;
			}
			OBSSourceAutoRelease current = obs_frontend_get_current_scene();
			if (current != target) {
				return; // A newer program request superseded this one.
			}
			self->SyncToMainScene(target, true, id.c_str());
		},
		Qt::QueuedConnection);
}

void DualCanvasController::SetProgramScene(obs_scene_t *scene, bool animate, const char *mainTransitionId)
{
	OBSCanvas canvas = PortraitCanvas();
	if (!canvas || !scene) {
		return;
	}
	const int mode = PortraitTransitionModeValue();
	bool fade = mode == static_cast<int>(PortraitTransitionMode::Fade);
	int duration = PortraitTransitionDuration();
	if (mode == static_cast<int>(PortraitTransitionMode::FollowMain)) {
		OBSSourceAutoRelease mainProgram = obs_get_output_source(0);
		const char *id = mainTransitionId ? mainTransitionId
						  : (mainProgram ? obs_source_get_unversioned_id(mainProgram) : "");
		// Only common built-ins have the same safe semantics in both orientations.
		fade = strcmp(id, "fade_transition") == 0;
		duration = std::clamp(obs_frontend_get_transition_duration(), 50, 20000);
	}
	obs_source_t *target = obs_scene_get_source(scene);
	if (!fade || !animate) {
		obs_canvas_set_channel(canvas, 0, target);
		if (portraitTransition) {
			obs_transition_clear(portraitTransition);
		}
		return;
	}
	if (!portraitTransition) {
		OBSSourceAutoRelease created =
			obs_source_create_private("fade_transition", "Studio Portrait Fade", nullptr);
		portraitTransition = created.Get();
		if (portraitTransition) {
			obs_transition_set_size(portraitTransition, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
			obs_transition_set_alignment(portraitTransition, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
			obs_transition_set_scale_type(portraitTransition, OBS_TRANSITION_SCALE_ASPECT);
		}
	}
	if (!portraitTransition) {
		blog(LOG_WARNING, "DualCanvas: Fade unavailable; cutting to the requested Portrait scene");
		obs_canvas_set_channel(canvas, 0, target);
		return;
	}
	OBSSourceAutoRelease previous = obs_canvas_get_channel(canvas, 0);
	if (previous != portraitTransition) {
		obs_transition_set(portraitTransition, previous);
		obs_canvas_set_channel(canvas, 0, portraitTransition);
	}
	if (!obs_transition_start(portraitTransition, OBS_TRANSITION_MODE_AUTO, duration, target)) {
		obs_transition_set(portraitTransition, target);
	}
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
	for (const char *name : {"item_add", "item_remove", "reorder", "refresh", "item_visible", "item_locked"}) {
		signal_handler_connect(handler, name, OnSceneItemSignal, this);
	}
	signal_handler_connect(handler, "remove", OnPortraitSceneRemoved, this);
	signalsConnected = true;
}

void DualCanvasController::DisconnectSceneSignals()
{
	if (!signalsConnected || !currentScene) {
		return;
	}
	signal_handler_t *handler = obs_source_get_signal_handler(obs_scene_get_source(currentScene));
	for (const char *name : {"item_add", "item_remove", "reorder", "refresh", "item_visible", "item_locked"}) {
		signal_handler_disconnect(handler, name, OnSceneItemSignal, this);
	}
	signal_handler_disconnect(handler, "remove", OnPortraitSceneRemoved, this);
	signalsConnected = false;
}

void DualCanvasController::OnPortraitSceneRemoved(void *data, calldata_t *params)
{
	/* Never mutate the scene graph inline in libobs' remove notification. A
	 * later root switch or Detach must also invalidate this recovery request. */
	QPointer<DualCanvasController> self(static_cast<DualCanvasController *>(data));
	auto *source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
	const QString removedUuid = QString::fromUtf8(obs_source_get_uuid(source));
	QMetaObject::invokeMethod(
		self.data(),
		[self, removedUuid]() {
			if (!self || !self->attached || !self->currentScene) {
				return;
			}
			auto *current = obs_scene_get_source(self->currentScene);
			if (!obs_source_removed(current) || removedUuid != obs_source_get_uuid(current)) {
				return;
			}
			self->SyncToProgramScene();
		},
		Qt::QueuedConnection);
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
	if (!currentScene || !source || obs_source_removed(source)) {
		return false;
	}
	if (obs_source_is_group(source)) {
		OBSCanvas canvas = PortraitCanvas();
		OBSScene destination = currentScene;
		if (!canvas) {
			return false;
		}
		bool added = false;
		ApplyStudioSceneEdit(destination, [this, source, &added, &canvas, &destination](obs_scene_t *scene) {
			PortraitGroupClonePlan clone(canvas);
			if (!clone.Prepare(source)) {
				return;
			}
			OBSSource copy = clone.Create(source);
			if (!copy || currentScene != destination || obs_source_removed(obs_scene_get_source(scene))) {
				return;
			}
			auto *item = obs_scene_add(scene, copy);
			if (!item) {
				return;
			}
			FitItemToCanvas(item, MultiStreamPortraitFit::Fit);
			clone.Commit();
			added = true;
		});
		if (!added) {
			blog(LOG_WARNING, "DualCanvas: could not create an independent Portrait group from '%s'",
			     obs_source_get_name(source));
		}
		return added;
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
	if (!currentScene) {
		return;
	}
	bool changed = false;
	struct Framing {
		MultiStreamPortraitFit fit;
		bool *changed;
	} framing{fit, &changed};
	obs_scene_enum_items(
		currentScene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (IsMirrorItem(item) && !obs_sceneitem_locked(item)) {
				auto &framing = *static_cast<Framing *>(param);
				FitItemToCanvas(item, framing.fit);
				*framing.changed = true;
			}
			return true;
		},
		&framing);
	if (changed) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(obs_scene_get_source(currentScene));
		obs_data_set_int(priv, StudioMirrorFitKey, static_cast<int>(fit));
	}
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
	MultiStreamPortraitFit fit = MultiStreamPortraitFit::Fill;
	OBSDataAutoRelease priv = obs_source_get_private_settings(obs_scene_get_source(currentScene));
	if (obs_data_has_user_value(priv, StudioMirrorFitKey)) {
		fit = obs_data_get_int(priv, StudioMirrorFitKey) == static_cast<int>(MultiStreamPortraitFit::Fit)
			      ? MultiStreamPortraitFit::Fit
			      : MultiStreamPortraitFit::Fill;
	} else {
		MirrorFit(fit);
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
		FitItemToCanvas(mirror, fit);
	}
}
