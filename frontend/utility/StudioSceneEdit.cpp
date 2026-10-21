#include "StudioSceneEdit.hpp"

#include <widgets/OBSBasic.hpp>
#include <components/SourceTree.hpp>
#include <set>
#include <vector>

namespace {
struct Dependencies {
	OBSDataArrayAutoRelease data{obs_data_array_create()};
	std::set<std::string> visited;
};
bool captureDependency(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto &state = *static_cast<Dependencies *>(param);
	auto *source = obs_sceneitem_get_source(item);
	if (!state.visited.insert(obs_source_get_uuid(source)).second) {
		return true;
	}
	if (obs_source_is_group(source)) {
		obs_scene_enum_items(obs_group_from_source(source), captureDependency, param);
	}
	OBSDataAutoRelease dependency = obs_data_create();
	OBSDataAutoRelease sourceData = obs_save_source(source);
	obs_data_set_obj(dependency, "data", sourceData);
	obs_data_set_bool(dependency, "private", obs_obj_is_private(source));
	obs_data_array_push_back(state.data, dependency);
	return true;
}
bool holdSource(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto *held = static_cast<std::vector<OBSSource> *>(data);
	held->emplace_back(obs_sceneitem_get_source(item));
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, holdSource, data);
	}
	return true;
}
} // namespace

void ApplyStudioSceneEdit(obs_scene_t *scene, const std::function<void(obs_scene_t *)> &change)
{
	// The renderer takes graphics before the scene lock. Item destruction may
	// also enter graphics, so the reverse order deadlocks during graph reload.
	struct GraphicsLock {
		GraphicsLock() { obs_enter_graphics(); }
		~GraphicsLock() { obs_leave_graphics(); }
	} graphics;
	auto operation = change;
	obs_scene_atomic_update(
		scene,
		[](void *data, obs_scene_t *root) { (*static_cast<std::function<void(obs_scene_t *)> *>(data))(root); },
		&operation);
}

OBSData CaptureStudioSceneEdit(obs_scene_t *scene)
{
	if (!scene) {
		return {};
	}
	Dependencies dependencies;
	obs_scene_enum_items(scene, captureDependency, &dependencies);
	OBSDataAutoRelease source = obs_save_source(obs_scene_get_source(scene));
	OBSDataAutoRelease settings = obs_data_get_obj(source, "settings");
	OBSDataAutoRelease priv = obs_source_get_private_settings(obs_scene_get_source(scene));
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_string(result, "scene_uuid", obs_source_get_uuid(obs_scene_get_source(scene)));
	obs_data_set_obj(result, "settings", settings);
	obs_data_set_array(result, "dependencies", dependencies.data);
	obs_data_set_bool(result, "has_mirror_fit", obs_data_has_user_value(priv, StudioMirrorFitKey));
	obs_data_set_int(result, "mirror_fit", obs_data_get_int(priv, StudioMirrorFitKey));
	int64_t selected = -1;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *id) {
			if (obs_sceneitem_selected(item)) {
				*static_cast<int64_t *>(id) = obs_sceneitem_get_id(item);
				return false;
			}
			return true;
		},
		&selected);
	obs_data_set_int(result, "selected_id", selected);
	OBSDataAutoRelease frozen = obs_data_create_from_json(obs_data_get_json(result));
	return frozen.Get();
}

bool StudioSceneEditChanged(obs_data_t *before, obs_data_t *after)
{
	OBSDataAutoRelease a = obs_data_get_obj(before, "settings"), b = obs_data_get_obj(after, "settings");
	return std::string(obs_data_get_json(a)) != obs_data_get_json(b) ||
	       obs_data_get_bool(before, "has_mirror_fit") != obs_data_get_bool(after, "has_mirror_fit") ||
	       obs_data_get_int(before, "mirror_fit") != obs_data_get_int(after, "mirror_fit");
}

bool RestoreStudioSceneEdit(const std::string &json, int64_t &selectedId)
{
	auto *main = OBSBasic::Get();
	if (!main || main->isClosing()) {
		return false;
	}
	OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
	OBSSourceAutoRelease source = obs_get_source_by_uuid(obs_data_get_string(data, "scene_uuid"));
	OBSScene scene = source ? obs_scene_from_source(source) : nullptr;
	if (!scene || obs_source_removed(source)) {
		return false;
	}
	// Retain old items' sources until the replacement graph holds them again.
	std::vector<OBSSource> held, created;
	obs_scene_enum_items(scene, holdSource, &held);
	OBSDataArrayAutoRelease dependencies = obs_data_get_array(data, "dependencies");
	for (size_t i = 0; i < obs_data_array_count(dependencies); ++i) {
		OBSDataAutoRelease dependency = obs_data_array_item(dependencies, i);
		OBSDataAutoRelease definition = obs_data_get_obj(dependency, "data");
		OBSSourceAutoRelease existing = obs_get_source_by_uuid(obs_data_get_string(definition, "uuid"));
		if (!existing) {
			existing = obs_data_get_bool(dependency, "private") ? obs_load_private_source(definition)
									    : obs_load_source(definition);
			if (!existing) {
				return false;
			}
			created.emplace_back(existing.Get());
		}
		held.emplace_back(existing.Get());
	}
	// Only newly created dependencies need loading. Reloading an existing
	// nested Main scene here would overwrite its independent composition.
	for (const auto &dependency : created) {
		obs_source_load2(dependency);
	}
	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	ApplyStudioSceneEdit(scene, [&source, &settings](obs_scene_t *) {
		obs_source_update(source, settings);
		obs_source_load(source);
	});
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	if (obs_data_get_bool(data, "has_mirror_fit")) {
		obs_data_set_int(priv, StudioMirrorFitKey, obs_data_get_int(data, "mirror_fit"));
	} else {
		obs_data_erase(priv, StudioMirrorFitKey);
	}
	main->SetStudioEditingScene(source);
	selectedId = obs_data_get_int(data, "selected_id");
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *id) {
			obs_sceneitem_select(item, obs_sceneitem_get_id(item) == *static_cast<int64_t *>(id));
			return true;
		},
		&selectedId);
	if (auto *tree = main->findChild<SourceTree *>("sources")) {
		tree->RefreshItems();
	}
	main->SaveProject();
	return true;
}
