#include "PortraitGroupEdit.hpp"
#include "StudioSceneEdit.hpp"

#include <graphics/matrix4.h>
#include <graphics/graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace {
using Identity = std::tuple<std::string, int64_t, std::string>;

struct Entry {
	OBSSceneItem item;
	size_t depth = 0;
};

std::vector<OBSSceneItem> children(obs_scene_t *scene)
{
	std::vector<OBSSceneItem> result;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			static_cast<std::vector<OBSSceneItem> *>(data)->emplace_back(item);
			return true;
		},
		&result);
	return result;
}

void collect(obs_sceneitem_t *item, size_t depth, std::vector<Entry> &result)
{
	if (!item || !obs_sceneitem_get_scene(item) || obs_source_removed(obs_sceneitem_get_source(item))) {
		return;
	}
	result.push_back({item, depth});
	if (obs_sceneitem_is_group(item)) {
		for (const auto &child : children(obs_sceneitem_group_get_scene(item))) {
			collect(child, depth + 1, result);
		}
	}
}

Identity identity(obs_sceneitem_t *item)
{
	return {obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(item))), obs_sceneitem_get_id(item),
		obs_source_get_uuid(obs_sceneitem_get_source(item))};
}

OBSData save(obs_sceneitem_t *item)
{
	OBSDataAutoRelease data = obs_data_create();
	const auto [owner, id, source] = identity(item);
	obs_data_set_string(data, "owner_uuid", owner.c_str());
	obs_data_set_int(data, "id", id);
	obs_data_set_string(data, "source_uuid", source.c_str());
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_get_crop(item, &crop);
	obs_data_set_vec2(data, "pos", &info.pos);
	obs_data_set_vec2(data, "scale", &info.scale);
	obs_data_set_double(data, "rot", info.rot);
	obs_data_set_int(data, "alignment", info.alignment);
	obs_data_set_int(data, "bounds_type", info.bounds_type);
	obs_data_set_int(data, "bounds_alignment", info.bounds_alignment);
	obs_data_set_bool(data, "crop_to_bounds", info.crop_to_bounds);
	obs_data_set_vec2(data, "bounds", &info.bounds);
	obs_data_set_int(data, "left", crop.left);
	obs_data_set_int(data, "right", crop.right);
	obs_data_set_int(data, "top", crop.top);
	obs_data_set_int(data, "bottom", crop.bottom);
	if (obs_sceneitem_is_group(item)) {
		auto *source = obs_sceneitem_get_source(item);
		obs_data_set_int(data, "source_width", obs_source_get_width(source));
		obs_data_set_int(data, "source_height", obs_source_get_height(source));
		matrix4 content;
		obs_sceneitem_get_content_transform(item, &content);
		vec2 x, y, origin;
		vec2_set(&x, content.x.x, content.x.y);
		vec2_set(&y, content.y.x, content.y.y);
		vec2_set(&origin, content.t.x, content.t.y);
		obs_data_set_vec2(data, "content_x", &x);
		obs_data_set_vec2(data, "content_y", &y);
		obs_data_set_vec2(data, "content_origin", &origin);
	}
	return data.Get();
}

void apply(obs_sceneitem_t *item, obs_data_t *data)
{
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	obs_data_get_vec2(data, "pos", &info.pos);
	obs_data_get_vec2(data, "scale", &info.scale);
	info.rot = float(obs_data_get_double(data, "rot"));
	info.alignment = uint32_t(obs_data_get_int(data, "alignment"));
	info.bounds_type = obs_bounds_type(obs_data_get_int(data, "bounds_type"));
	info.bounds_alignment = uint32_t(obs_data_get_int(data, "bounds_alignment"));
	info.crop_to_bounds = obs_data_get_bool(data, "crop_to_bounds");
	obs_data_get_vec2(data, "bounds", &info.bounds);
	crop.left = int(obs_data_get_int(data, "left"));
	crop.right = int(obs_data_get_int(data, "right"));
	crop.top = int(obs_data_get_int(data, "top"));
	crop.bottom = int(obs_data_get_int(data, "bottom"));
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &info);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_defer_update_end(item);
	// Group children defer transform updates even after defer_update_end.
	obs_sceneitem_force_update_transform(item);
}

void applySettledGroup(obs_sceneitem_t *item, obs_data_t *data, const vec2 &rebase)
{
	apply(item, data);
	auto *source = obs_sceneitem_get_source(item);
	if (!obs_data_has_user_value(data, "source_width") ||
	    (obs_data_get_int(data, "source_width") == obs_source_get_width(source) &&
	     obs_data_get_int(data, "source_height") == obs_source_get_height(source) && rebase.x == 0 &&
	     rebase.y == 0)) {
		return;
	}
	// A removed child (or a surviving source whose dimensions changed) can
	// leave a smaller group. Restoring its old center/right-aligned position
	// verbatim would move all survivors. Preserve the saved source-pixel frame
	// instead; no absent item or source is reconstructed.
	vec2 savedX, savedY, savedOrigin;
	obs_data_get_vec2(data, "content_x", &savedX);
	obs_data_get_vec2(data, "content_y", &savedY);
	obs_data_get_vec2(data, "content_origin", &savedOrigin);
	// Removing the left/top child rebases surviving local coordinates to zero.
	// Preserve their old source-pixel locations in the saved content frame.
	savedOrigin.x += savedX.x * rebase.x + savedY.x * rebase.y;
	savedOrigin.y += savedX.y * rebase.x + savedY.y * rebase.y;
	matrix4 current;
	obs_sceneitem_get_content_transform(item, &current);
	obs_transform_info info{};
	obs_sceneitem_get_info2(item, &info);
	if (info.bounds_type != OBS_BOUNDS_NONE) {
		// A bounded group deliberately refits its current content into the
		// saved box. Preserve that box/position instead of changing its bounds
		// or moving it to imitate the old source-pixel frame.
		return;
	}
	const double x = double(info.pos.x) + double(savedOrigin.x) - current.t.x;
	const double y = double(info.pos.y) + double(savedOrigin.y) - current.t.y;
	if (std::isfinite(x) && std::isfinite(y) && std::abs(x) <= std::numeric_limits<float>::max() &&
	    std::abs(y) <= std::numeric_limits<float>::max()) {
		info.pos.x = float(x);
		info.pos.y = float(y);
		obs_sceneitem_set_info2(item, &info);
		obs_sceneitem_force_update_transform(item);
	}
}

void traceRestore(const char *phase, const char *reason, obs_scene_t *root, obs_sceneitem_t *anchor,
		  obs_data_t *snapshot, size_t matched = 0, size_t missing = 0, size_t disallowed = 0)
{
	if (!std::getenv("OBS_STUDIO_PORTRAIT_GROUPS_ROOT")) {
		return;
	}
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "phase", phase);
	obs_data_set_string(data, "reason", reason);
	obs_data_set_string(data, "expected_root", snapshot ? obs_data_get_string(snapshot, "root_uuid") : "");
	obs_data_set_string(data, "actual_root", root ? obs_source_get_uuid(obs_scene_get_source(root)) : "");
	obs_data_set_int(data, "matched", int64_t(matched));
	obs_data_set_int(data, "missing", int64_t(missing));
	obs_data_set_int(data, "disallowed_owner", int64_t(disallowed));
	if (root) {
		OBSCanvasAutoRelease canvas = obs_source_get_canvas(obs_scene_get_source(root));
		obs_data_set_string(data, "root_canvas", canvas ? obs_canvas_get_uuid(canvas) : "");
	}
	if (snapshot) {
		OBSDataArrayAutoRelease items = obs_data_get_array(snapshot, "items");
		obs_data_set_int(data, "saved_items", int64_t(obs_data_array_count(items)));
		if (obs_data_array_count(items)) {
			OBSDataAutoRelease savedAnchor = obs_data_array_item(items, 0);
			obs_data_set_obj(data, "expected_anchor", savedAnchor);
		}
	}
	if (anchor && obs_sceneitem_get_scene(anchor)) {
		OBSData actual = save(anchor);
		obs_data_set_obj(data, "actual_anchor", actual);
	}
	blog(LOG_INFO, "[portrait-group-restore] %s", obs_data_get_json(data));
}
} // namespace

FreezePortraitGroups::FreezePortraitGroups(const std::vector<OBSSceneItem> &groups)
{
	std::set<obs_sceneitem_t *> seen;
	for (const auto &group : groups) {
		if (group && obs_sceneitem_is_group(group) && seen.insert(group.Get()).second) {
			held.push_back(group);
			obs_sceneitem_defer_group_resize_begin(group);
		}
	}
}

FreezePortraitGroups::~FreezePortraitGroups()
{
	for (const auto &group : held) {
		obs_sceneitem_defer_group_resize_end(group);
	}
}

void FreezePortraitGroups::Release(obs_sceneitem_t *group)
{
	const auto entry = std::find_if(held.begin(), held.end(),
					[group](const OBSSceneItem &item) { return item.Get() == group; });
	if (entry != held.end()) {
		obs_sceneitem_defer_group_resize_end(*entry);
		held.erase(entry);
	}
}

void FreezePortraitGroups::ReleaseAndSettle(obs_scene_t *root)
{
	auto release = [this](obs_scene_t *scene) {
		if (!scene) {
			for (const auto &group : held) {
				obs_sceneitem_defer_group_resize_end(group);
			}
			held.clear();
			return;
		}
		std::vector<Entry> attached;
		for (const auto &item : children(scene)) {
			collect(item, 0, attached);
		}
		std::map<obs_sceneitem_t *, size_t> depths;
		for (const auto &entry : attached) {
			depths[entry.item.Get()] = entry.depth;
		}
		auto order = held;
		std::stable_sort(order.begin(), order.end(), [&depths](const OBSSceneItem &a, const OBSSceneItem &b) {
			const auto x = depths.find(a.Get()), y = depths.find(b.Get());
			return (x == depths.end() ? std::numeric_limits<size_t>::max() : x->second) >
			       (y == depths.end() ? std::numeric_limits<size_t>::max() : y->second);
		});
		bool pruned = false;
		for (const auto &group : order) {
			const bool eligible = depths.count(group.Get()) && obs_sceneitem_get_scene(group) &&
					      !obs_source_removed(obs_sceneitem_get_source(group));
			const auto owner = obs_sceneitem_group_get_scene(group);
			OBSSceneItem reference;
			vec2 oldPosition{};
			matrix4 oldContent{};
			obs_transform_info info{};
			if (eligible) {
				obs_sceneitem_force_update_transform(group);
				obs_sceneitem_get_info2(group, &info);
				obs_sceneitem_get_content_transform(group, &oldContent);
				for (const auto &child : children(owner)) {
					if (!obs_source_removed(obs_sceneitem_get_source(child))) {
						reference = child;
						obs_sceneitem_get_pos(child, &oldPosition);
						break;
					}
				}
			}
			Release(group);
			if (!eligible) {
				continue;
			}
			// Ancestors remain frozen until their descendants have settled.
			obs_scene_prune_sources(scene);
			pruned = true;
			if (info.bounds_type != OBS_BOUNDS_NONE || !reference ||
			    obs_sceneitem_get_scene(reference) != owner || !obs_sceneitem_get_scene(group) ||
			    obs_source_removed(obs_sceneitem_get_source(group))) {
				continue;
			}
			vec2 position, rebase;
			obs_sceneitem_get_pos(reference, &position);
			vec2_sub(&rebase, &oldPosition, &position);
			matrix4 content;
			obs_sceneitem_get_content_transform(group, &content);
			obs_sceneitem_get_info2(group, &info);
			// resize_group uses raw extents for alignment while scene dimensions
			// are ceiled. Preserve the pre-resize source-pixel frame explicitly,
			// including the local rebase, rather than accumulating that drift.
			const double x = double(info.pos.x) + oldContent.t.x + oldContent.x.x * double(rebase.x) +
					 oldContent.y.x * double(rebase.y) - content.t.x;
			const double y = double(info.pos.y) + oldContent.t.y + oldContent.x.y * double(rebase.x) +
					 oldContent.y.y * double(rebase.y) - content.t.y;
			if (std::isfinite(x) && std::isfinite(y) && std::abs(x) <= std::numeric_limits<float>::max() &&
			    std::abs(y) <= std::numeric_limits<float>::max()) {
				info.pos.x = float(x);
				info.pos.y = float(y);
				obs_sceneitem_set_info2(group, &info);
				obs_sceneitem_force_update_transform(group);
			}
		}
		if (!pruned) {
			obs_scene_prune_sources(scene);
		}
	};
	if (root) {
		ApplyStudioSceneEdit(root, release);
	} else {
		release(nullptr);
	}
}

std::string CapturePortraitGroupTransforms(obs_scene_t *root, obs_sceneitem_t *topGroup)
{
	if (!root || !topGroup || !obs_sceneitem_is_group(topGroup) || obs_sceneitem_get_scene(topGroup) != root) {
		return {};
	}
	std::string result;
	ApplyStudioSceneEdit(root, [&result, topGroup](obs_scene_t *scene) {
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "root_uuid", obs_source_get_uuid(obs_scene_get_source(scene)));
		OBSDataArrayAutoRelease array = obs_data_array_create();
		std::vector<Entry> entries;
		collect(topGroup, 0, entries);
		for (const auto &entry : entries) {
			OBSData saved = save(entry.item);
			obs_data_array_push_back(array, saved);
		}
		obs_data_set_array(data, "items", array);
		result = obs_data_get_json(data);
	});
	return result;
}

bool RestorePortraitGroupTransforms(const std::string &json, FreezePortraitGroups *releaseFirst)
{
	auto reject = [releaseFirst](obs_scene_t *root) {
		if (releaseFirst) {
			releaseFirst->ReleaseAndSettle(root);
		}
		return false;
	};
	// Scanning Main while a caller holds the Portrait root reverses the lock
	// order of a Main scene that references it. Do not silently allow nesting.
	if (gs_get_context()) {
		traceRestore("reject", "graphics_context", nullptr, nullptr, nullptr);
		blog(LOG_WARNING, "Portrait group restore requires an outer preflight before the graphics lock");
		return reject(nullptr);
	}
	OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
	if (!data) {
		traceRestore("reject", "invalid_json", nullptr, nullptr, nullptr);
		return reject(nullptr);
	}
	OBSSourceAutoRelease source = obs_get_source_by_uuid(obs_data_get_string(data, "root_uuid"));
	OBSScene root = source ? obs_scene_from_source(source) : nullptr;
	if (!root || obs_source_removed(source)) {
		traceRestore("reject", "root_missing_or_removed", root, nullptr, data);
		return reject(nullptr);
	}
	OBSDataArrayAutoRelease saved = obs_data_get_array(data, "items");
	if (!obs_data_array_count(saved)) {
		traceRestore("reject", "empty_items", root, nullptr, data);
		return reject(root);
	}
	OBSDataAutoRelease savedAnchor = obs_data_array_item(saved, 0);
	const Identity anchorIdentity{obs_data_get_string(savedAnchor, "owner_uuid"),
				      obs_data_get_int(savedAnchor, "id"),
				      obs_data_get_string(savedAnchor, "source_uuid")};
	OBSSceneItem anchor = obs_scene_find_sceneitem_by_id(root, obs_data_get_int(savedAnchor, "id"));
	if (!anchor || !obs_sceneitem_is_group(anchor) || identity(anchor) != anchorIdentity ||
	    obs_source_removed(obs_sceneitem_get_source(anchor))) {
		traceRestore("reject", "anchor_identity", root, anchor, data);
		return reject(root);
	}
	// A reference can be added to Main after the original gesture. Undo must
	// not start editing its child scene through that newly shared reference.
	if (PortraitGroupsSharedWithMain(root, {anchor})) {
		traceRestore("reject", "shared_with_main", root, anchor, data);
		return reject(root);
	}
	traceRestore("preflight", "accepted", root, anchor, data);
	bool changed = false;
	ApplyStudioSceneEdit(root, [&data, &saved, &anchor, &anchorIdentity, &changed,
				    releaseFirst](obs_scene_t *scene) {
		if (releaseFirst) {
			releaseFirst->ReleaseAndSettle(scene);
		}
		if (obs_sceneitem_get_scene(anchor) != scene || identity(anchor) != anchorIdentity ||
		    obs_source_removed(obs_sceneitem_get_source(anchor))) {
			traceRestore("reject", "anchor_changed_after_release", scene, anchor, data);
			return;
		}
		std::vector<Entry> available;
		collect(anchor, 0, available);
		struct Change {
			Entry entry;
			OBSData data;
		};
		std::vector<Change> changes;
		std::vector<OBSSceneItem> groups;
		std::set<std::string> allowedOwners{obs_source_get_uuid(obs_scene_get_source(scene))};
		size_t missing = 0, disallowed = 0;
		for (size_t i = 0; i < obs_data_array_count(saved); ++i) {
			OBSDataAutoRelease item = obs_data_array_item(saved, i);
			if (!allowedOwners.count(obs_data_get_string(item, "owner_uuid"))) {
				++disallowed;
				continue;
			}
			const Identity target{obs_data_get_string(item, "owner_uuid"), obs_data_get_int(item, "id"),
					      obs_data_get_string(item, "source_uuid")};
			auto match = std::find_if(available.begin(), available.end(), [&target](const Entry &entry) {
				return identity(entry.item) == target;
			});
			if (match == available.end()) {
				++missing;
				continue; // Never recreate removed items or follow an item to another owner.
			}
			changes.push_back({*match, item.Get()});
			if (obs_sceneitem_is_group(match->item)) {
				groups.push_back(match->item);
				// Capture stores ancestors before descendants. A group's child
				// owner is admitted only if that group itself still belongs to
				// its captured owner; do not follow a reparented nested group.
				allowedOwners.insert(obs_source_get_uuid(obs_sceneitem_get_source(match->item)));
			}
		}
		std::stable_sort(changes.begin(), changes.end(),
				 [](const Change &a, const Change &b) { return a.entry.depth > b.entry.depth; });
		if (changes.empty()) {
			traceRestore("reject", "no_matching_items", scene, anchor, data, 0, missing, disallowed);
			return;
		}
		traceRestore("apply", "matching_items", scene, anchor, data, changes.size(), missing, disallowed);
		FreezePortraitGroups freeze(groups);
		for (const auto &change : changes) {
			apply(change.entry.item, change.data);
		}
		// Settle dimensions from the innermost group out. Its old source size
		// otherwise shifts centered/right-aligned group positions during resize.
		for (const auto &change : changes) {
			if (obs_sceneitem_is_group(change.entry.item)) {
				const auto owner = obs_sceneitem_group_get_scene(change.entry.item);
				OBSSceneItem reference;
				vec2 oldPosition{};
				for (const auto &child : children(owner)) {
					if (!obs_source_removed(obs_sceneitem_get_source(child))) {
						reference = child;
						obs_sceneitem_get_pos(child, &oldPosition);
						break;
					}
				}
				freeze.Release(change.entry.item);
				obs_scene_prune_sources(scene);
				vec2 rebase{};
				if (reference && obs_sceneitem_get_scene(reference) == owner) {
					vec2 position;
					obs_sceneitem_get_pos(reference, &position);
					vec2_sub(&rebase, &oldPosition, &position);
				}
				applySettledGroup(change.entry.item, change.data, rebase);
			}
		}
		obs_scene_prune_sources(scene);
		changed = !changes.empty();
		traceRestore("complete", "restored", scene, anchor, data, changes.size(), missing, disallowed);
	});
	return changed;
}

bool PortraitGroupsSharedWithMain(obs_scene_t *root, const std::vector<OBSSceneItem> &ancestors)
{
	if (!root || ancestors.empty()) {
		return false;
	}
	OBSCanvasAutoRelease rootCanvas = obs_source_get_canvas(obs_scene_get_source(root));
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	if (rootCanvas == mainCanvas) {
		return false;
	}
	std::set<obs_source_t *> targets;
	// History also restores sibling transforms in the top-level branch.
	// A shared nested sibling needs the same protection as an ancestor.
	std::vector<Entry> branch;
	collect(ancestors.front(), 0, branch);
	for (const auto &entry : branch) {
		if (obs_sceneitem_is_group(entry.item)) {
			targets.insert(obs_sceneitem_get_source(entry.item));
		}
	}
	std::vector<OBSSource> mainScenes;
	obs_enum_scenes(
		[](void *data, obs_source_t *source) {
			if (!obs_source_removed(source)) {
				static_cast<std::vector<OBSSource> *>(data)->emplace_back(source);
			}
			return true;
		},
		&mainScenes);
	std::set<obs_scene_t *> visited;
	std::function<bool(obs_scene_t *)> shared = [&](obs_scene_t *scene) {
		if (!scene || !visited.insert(scene).second) {
			return false;
		}
		for (const auto &item : children(scene)) {
			if (!obs_sceneitem_is_group(item)) {
				continue;
			}
			if (targets.count(obs_sceneitem_get_source(item)) ||
			    shared(obs_sceneitem_group_get_scene(item))) {
				return true;
			}
		}
		return false;
	};
	for (const auto &scene : mainScenes) {
		if (shared(obs_scene_from_source(scene))) {
			if (std::getenv("OBS_STUDIO_PORTRAIT_GROUPS_ROOT")) {
				blog(LOG_INFO, "[portrait-group-sharing] root=%s main_scene=%s main_uuid=%s",
				     obs_source_get_uuid(obs_scene_get_source(root)), obs_source_get_name(scene),
				     obs_source_get_uuid(scene));
			}
			return true;
		}
	}
	return false;
}
