#pragma once

#include <obs.hpp>
#include <string>
#include <vector>

class FreezePortraitGroups;

/* Transform-only history for one top-level group and its entire subtree.
 * Autosizing rebases siblings, so this scope intentionally includes their
 * transforms. It never restores sources, settings, filters or graph structure.
 * Items are addressed by root UUID, owner UUID, item ID and source UUID.
 * If children disappeared, unbounded group positions compensate the new size
 * and local rebase to retain the surviving source-pixel frame. Bounded groups
 * retain their saved box and follow the native refit of the current content. */
std::string CapturePortraitGroupTransforms(obs_scene_t *root, obs_sceneitem_t *topGroup);
/* Call outside a graphics/scene transaction: the Main-sharing preflight must
 * precede the root lock. releaseFirst atomically ends an in-progress gesture
 * before restoration; rejected restores still release every freeze counter. */
bool RestorePortraitGroupTransforms(const std::string &json, FreezePortraitGroups *releaseFirst = nullptr);

/* Balances every begin, including removed groups. Call ReleaseAndSettle while
 * the original root still exists to settle dimensions before saving history.
 * The destructor releases remaining deferrals; it does not prune a stale root. */
class FreezePortraitGroups {
public:
	explicit FreezePortraitGroups(const std::vector<OBSSceneItem> &groups);
	~FreezePortraitGroups();
	FreezePortraitGroups(const FreezePortraitGroups &) = delete;
	FreezePortraitGroups &operator=(const FreezePortraitGroups &) = delete;
	void Release(obs_sceneitem_t *group);
	void ReleaseAndSettle(obs_scene_t *root);

private:
	std::vector<OBSSceneItem> held;
};

/* Legacy group references may still share their child scene with Main.
 * Call before editing their children; ordinary leaf references are safe. */
bool PortraitGroupsSharedWithMain(obs_scene_t *root, const std::vector<OBSSceneItem> &ancestors);
