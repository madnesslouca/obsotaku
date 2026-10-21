#include "PortraitPreviewEditor.hpp"
#include "StudioSceneEdit.hpp"
#include "PortraitTransformGeometry.hpp"
#include "PortraitGroupEdit.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <graphics/graphics.h>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <QToolTip>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace {
constexpr double width = 1080, height = 1920;
constexpr int rotationHandleIndex = 8;
constexpr double pi = 3.14159265358979323846;
// Clockwise: corners alternate with edge midpoints.
const std::array<QPointF, 8> handles = {QPointF(0, 0), QPointF(.5, 0), QPointF(1, 0), QPointF(1, .5),
					QPointF(1, 1), QPointF(.5, 1), QPointF(0, 1), QPointF(0, .5)};
QPointF map(const matrix4 &matrix, QPointF point)
{
	vec3 in{}, out{};
	vec3_set(&in, point.x(), point.y(), 0);
	vec3_transform(&out, &in, &matrix);
	return {out.x, out.y};
}
std::optional<std::array<QPointF, 3>> rotationGrip(const matrix4 &box, double distance, double radius)
{
	const QPointF center = map(box, {.5, .5});
	const QRectF visible(radius, radius, width - 2 * radius, height - 2 * radius);
	// Prefer an outward stem. When the box fills the canvas, use an inward
	// grip so it remains visible inside the renderer's canvas viewport.
	for (double direction : {1., -1.}) {
		for (QPointF midpoint : {QPointF(.5, 0), QPointF(1, .5), QPointF(.5, 1), QPointF(0, .5)}) {
			const QPointF edge = map(box, midpoint);
			const QPointF delta = edge - center;
			const double length = std::hypot(delta.x(), delta.y());
			if (!std::isfinite(length) || length < 1e-6) {
				continue;
			}
			const QPointF grip = edge + delta * (direction * distance / length);
			if (visible.contains(grip)) {
				return std::array<QPointF, 3>{edge, grip, center};
			}
		}
	}
	return std::nullopt;
}
bool editable(obs_sceneitem_t *item)
{
	return item && obs_sceneitem_get_scene(item) && !obs_source_removed(obs_sceneitem_get_source(item)) &&
	       obs_sceneitem_visible(item) && !obs_sceneitem_locked(item) &&
	       (obs_source_get_output_flags(obs_sceneitem_get_source(item)) & OBS_SOURCE_VIDEO);
}
std::vector<OBSSceneItem> items(obs_scene_t *scene)
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
struct Scope {
	OBSScene owner;
	std::vector<OBSSceneItem> ancestors;
	matrix4 toCanvas{};
	bool enabled = true;
};
Scope scopeFor(obs_scene_t *root, const QString &uuid)
{
	Scope result;
	matrix4_identity(&result.toCanvas);
	if (!root) {
		return result;
	}
	if (uuid.isEmpty() || uuid == QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(root)))) {
		result.owner = root;
		return result;
	}
	std::set<std::string> visited;
	std::function<bool(obs_scene_t *, const Scope &)> find = [&](obs_scene_t *owner, const Scope &parent) {
		for (const auto &item : items(owner)) {
			if (!obs_sceneitem_is_group(item)) {
				continue;
			}
			auto *source = obs_sceneitem_get_source(item);
			const std::string id = obs_source_get_uuid(source);
			if (!visited.insert(id).second) {
				continue;
			}
			Scope child = parent;
			child.owner = obs_sceneitem_group_get_scene(item);
			child.ancestors.push_back(item);
			child.enabled = child.enabled && editable(item);
			matrix4 content;
			obs_sceneitem_get_content_transform(item, &content);
			matrix4_mul(&child.toCanvas, &content, &parent.toCanvas);
			if (uuid == QString::fromStdString(id)) {
				result = child;
				return true;
			}
			if (find(child.owner, child)) {
				return true;
			}
		}
		return false;
	};
	find(root, result);
	return result;
}
matrix4 worldBox(obs_sceneitem_t *item, const matrix4 &parent)
{
	matrix4 local, result;
	obs_sceneitem_get_box_transform(item, &local);
	matrix4_mul(&result, &local, &parent);
	return result;
}
QPointF vectorMap(const matrix4 &matrix, QPointF delta)
{
	return map(matrix, delta) - map(matrix, {0, 0});
}
bool sameTransform(const obs_transform_info &a, const obs_transform_info &b)
{
	return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.scale.x == b.scale.x && a.scale.y == b.scale.y &&
	       a.rot == b.rot && a.alignment == b.alignment && a.bounds_type == b.bounds_type &&
	       a.bounds_alignment == b.bounds_alignment && a.bounds.x == b.bounds.x && a.bounds.y == b.bounds.y &&
	       a.crop_to_bounds == b.crop_to_bounds;
}
bool sameCrop(const obs_sceneitem_crop &a, const obs_sceneitem_crop &b)
{
	return a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}
QJsonObject traceItem(obs_sceneitem_t *item)
{
	if (!item) {
		return {};
	}
	auto *source = obs_sceneitem_get_source(item);
	auto *owner = obs_sceneitem_get_scene(item);
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_get_crop(item, &crop);
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	return {{"name", QString::fromUtf8(obs_source_get_name(source))},
		{"owner", owner ? QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(owner))) : QString{}},
		{"id", double(obs_sceneitem_get_id(item))},
		{"source", QString::fromUtf8(obs_source_get_uuid(source))},
		{"source_width", int(obs_source_get_width(source))},
		{"source_height", int(obs_source_get_height(source))},
		{"selected", obs_sceneitem_selected(item)},
		{"visible", obs_sceneitem_visible(item)},
		{"locked", obs_sceneitem_locked(item)},
		{"source_removed", obs_source_removed(source)},
		{"pos", QJsonArray{info.pos.x, info.pos.y}},
		{"scale", QJsonArray{info.scale.x, info.scale.y}},
		{"rotation", info.rot},
		{"bounds_type", int(info.bounds_type)},
		{"bounds", QJsonArray{info.bounds.x, info.bounds.y}},
		{"crop", QJsonArray{crop.left, crop.top, crop.right, crop.bottom}},
		{"box", QJsonArray{box.x.x, box.x.y, box.y.x, box.y.y, box.t.x, box.t.y}}};
}
bool insideAncestors(const Scope &scope, QPointF point)
{
	matrix4 parent;
	matrix4_identity(&parent);
	for (const auto &group : scope.ancestors) {
		matrix4 content, world, inverse;
		obs_sceneitem_get_content_transform(group, &content);
		matrix4_mul(&world, &content, &parent);
		if (!matrix4_inv(&inverse, &world)) {
			return false;
		}
		const auto local = map(inverse, point);
		obs_sceneitem_crop crop{};
		obs_sceneitem_get_effective_crop(group, &crop);
		auto *source = obs_sceneitem_get_source(group);
		if (local.x() < crop.left || local.y() < crop.top ||
		    local.x() > double(obs_source_get_width(source)) - crop.right ||
		    local.y() > double(obs_source_get_height(source)) - crop.bottom) {
			return false;
		}
		parent = world;
	}
	return true;
}
std::vector<OBSSceneItem> selectedItems(obs_scene_t *scene)
{
	auto result = items(scene);
	result.erase(std::remove_if(result.begin(), result.end(),
				    [](const OBSSceneItem &item) {
					    return !editable(item) || !obs_sceneitem_selected(item);
				    }),
		     result.end());
	return result;
}
QRectF bounds(const std::vector<OBSSceneItem> &layers, const matrix4 &parent)
{
	constexpr double infinity = std::numeric_limits<double>::infinity();
	double left = infinity, top = infinity, right = -infinity, bottom = -infinity;
	for (const auto &layer : layers) {
		const matrix4 transform = worldBox(layer, parent);
		for (size_t i : {0u, 2u, 4u, 6u}) {
			const QPointF point = map(transform, handles[i]);
			if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
				return {};
			}
			left = std::min(left, point.x());
			right = std::max(right, point.x());
			top = std::min(top, point.y());
			bottom = std::max(bottom, point.y());
		}
	}
	return layers.empty() ? QRectF{} : QRectF(left, top, right - left, bottom - top);
}
std::array<QPointF, 8> frameHandles(const QRectF &frame)
{
	std::array<QPointF, 8> points;
	for (size_t i = 0; i < handles.size(); ++i) {
		points[i] = frame.topLeft() + QPointF(handles[i].x() * frame.width(), handles[i].y() * frame.height());
	}
	return points;
}
} // namespace

PortraitPreviewEditor::PortraitPreviewEditor(QWidget *display, std::function<OBSScene()> scene,
					     std::function<void(const QString &, int64_t, bool, bool)> select)
	: QObject(display),
	  display(display),
	  scene(std::move(scene)),
	  select(std::move(select))
{
	display->setMouseTracking(true);
	display->setFocusPolicy(Qt::StrongFocus);
	display->installEventFilter(this);
	display->setToolTip(QTStr("Studio.Portrait.Gestures"));
	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
	timer->start(33);
}

void PortraitPreviewEditor::SetCropMode(bool enabled)
{
	Cancel();
	cropMode = enabled;
	Refresh();
}

void PortraitPreviewEditor::SetGroupScope(const QString &uuid)
{
	groupScope = uuid;
	display->setProperty("studioPortraitGroupScope", uuid);
}

void PortraitPreviewEditor::AdoptSelection()
{
	if (active) {
		return;
	}
	OBSScene current = scene();
	if (!current) {
		SetGroupScope({});
		return;
	}
	const auto rootItems = items(current);
	if (std::any_of(rootItems.begin(), rootItems.end(),
			[](const OBSSceneItem &item) { return editable(item) && obs_sceneitem_selected(item); })) {
		SetGroupScope({});
		return;
	}
	std::function<QString(obs_scene_t *)> find = [&](obs_scene_t *owner) -> QString {
		for (const auto &item : items(owner)) {
			if (editable(item) && obs_sceneitem_selected(item)) {
				return QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(owner)));
			}
		}
		for (const auto &item : items(owner)) {
			if (editable(item) && obs_sceneitem_is_group(item)) {
				const QString found = find(obs_sceneitem_group_get_scene(item));
				if (!found.isEmpty()) {
					return found;
				}
			}
		}
		return {};
	};
	const QString selectedOwner = find(current);
	if (!selectedOwner.isEmpty()) {
		SetGroupScope(selectedOwner);
	} else if (!scopeFor(current, groupScope).owner) {
		SetGroupScope({});
	}
}

bool PortraitPreviewEditor::EnterGroup(QPointF point)
{
	Cancel();
	AdoptSelection();
	const Scope scope = scopeFor(scene(), groupScope);
	if (!scope.owner || !scope.enabled) {
		return false;
	}
	int ignored;
	OBSSceneItem target = Hit(CanvasPoint(point), ignored);
	if (!target || !editable(target) || !obs_sceneitem_is_group(target)) {
		return false;
	}
	auto ancestors = scope.ancestors;
	ancestors.emplace_back(target);
	if (PortraitGroupsSharedWithMain(scene(), ancestors)) {
		Feedback("shared_group");
		return false;
	}
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(obs_sceneitem_get_source(target)));
	SetGroupScope(uuid);
	select(uuid, -1, false, false);
	display->setFocus(Qt::MouseFocusReason);
	Refresh();
	return true;
}

void PortraitPreviewEditor::ExitGroup()
{
	Cancel();
	const Scope scope = scopeFor(scene(), groupScope);
	if (scope.ancestors.empty()) {
		SetGroupScope({});
		Refresh();
		return;
	}
	const auto group = scope.ancestors.back();
	const QString parent =
		QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(group))));
	const QString rootUuid = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(scene())));
	SetGroupScope(parent == rootUuid ? QString{} : parent);
	select(parent, obs_sceneitem_get_id(group), false, false);
	Refresh();
}

PortraitPreviewEditor::~PortraitPreviewEditor()
{
	Cancel();
}

QRectF PortraitPreviewEditor::Frame() const
{
	// Match the renderer's integer physical-pixel viewport, including HiDPI.
	const double dpr = display->devicePixelRatioF();
	const int w = qRound(display->width() * dpr), h = qRound(display->height() * dpr);
	const double scale = std::min(w / width, h / height);
	const int fw = std::max(1, int(width * scale)), fh = std::max(1, int(height * scale));
	return {double((w - fw) / 2) / dpr, double((h - fh) / 2) / dpr, fw / dpr, fh / dpr};
}

QPointF PortraitPreviewEditor::CanvasPoint(QPointF point) const
{
	const QRectF frame = Frame();
	return {(point.x() - frame.x()) * width / frame.width(), (point.y() - frame.y()) * height / frame.height()};
}

OBSSceneItem PortraitPreviewEditor::Hit(QPointF point, int &hitHandle) const
{
	hitHandle = -1;
	const Scope scope = scopeFor(scene(), groupScope);
	if (!scope.owner || !scope.enabled) {
		return nullptr;
	}
	auto layers = items(scope.owner);
	const double radius = 7 * width / Frame().width();
	const auto selection = selectedItems(scope.owner);
	if (selection.size() > 1) {
		const auto frame = bounds(selection, scope.toCanvas);
		if (frame.isValid()) {
			const auto points = frameHandles(frame);
			for (size_t i = 0; i < points.size(); ++i) {
				if (QLineF(points[i], point).length() <= radius) {
					hitHandle = int(i);
					return selection.back();
				}
			}
		}
	}
	if (selection.size() == 1 && !PortraitGroupsSharedWithMain(scene(), scope.ancestors)) {
		const auto control =
			rotationGrip(worldBox(selection.front(), scope.toCanvas), 24 * width / Frame().width(), radius);
		if (control && QLineF((*control)[1], point).length() <= radius) {
			hitHandle = rotationHandleIndex;
			return selection.front();
		}
	}
	// Selected handles take precedence over bodies, just like the Main editor.
	for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
		if (selection.size() != 1 || !editable(*it) || !obs_sceneitem_selected(*it)) {
			continue;
		}
		const matrix4 transform = worldBox(*it, scope.toCanvas);
		for (size_t i = 0; i < handles.size(); ++i) {
			QPointF delta = map(transform, handles[i]) - point;
			if (std::hypot(delta.x(), delta.y()) <= radius) {
				hitHandle = int(i);
				return *it;
			}
		}
	}
	if (!insideAncestors(scope, point)) {
		return nullptr;
	}
	for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
		if (!editable(*it)) {
			continue;
		}
		matrix4 inv;
		const matrix4 transform = worldBox(*it, scope.toCanvas);
		if (!matrix4_inv(&inv, &transform)) {
			continue;
		}
		QPointF local = map(inv, point);
		// Matrix inversion and logical/physical pixel conversion can put an
		// exact border a few float ULPs outside the unit box.
		constexpr double edgeTolerance = 1e-5;
		if (local.x() >= -edgeTolerance && local.x() <= 1 + edgeTolerance && local.y() >= -edgeTolerance &&
		    local.y() <= 1 + edgeTolerance) {
			return *it;
		}
	}
	return nullptr;
}

void PortraitPreviewEditor::Begin(QPointF point, Qt::KeyboardModifiers modifiers)
{
	Trace("begin_request",
	      QStringLiteral("logical=%1,%2 modifiers=%3").arg(point.x()).arg(point.y()).arg(int(modifiers)));
	Cancel();
	if (!scene()) {
		Trace("begin_rejected", "no_scene");
		return;
	}
	ApplyStudioSceneEdit(scene(), [](obs_scene_t *root) { obs_scene_prune_sources(root); });
	AdoptSelection();
	const Scope scope = scopeFor(scene(), groupScope);
	if (!scope.owner || !scope.enabled || !matrix4_inv(&canvasToParent, &scope.toCanvas)) {
		Trace("begin_rejected", "invalid_scope");
		return;
	}
	if (PortraitGroupsSharedWithMain(scene(), scope.ancestors)) {
		Trace("begin_rejected", "shared_group");
		Feedback("shared_group");
		return;
	}
	const QPointF canvas = CanvasPoint(point);
	int hitHandle;
	OBSSceneItem hit = Hit(canvas, hitHandle);
	Trace("begin_hit", QStringLiteral("canvas=%1,%2 handle=%3").arg(canvas.x()).arg(canvas.y()).arg(hitHandle),
	      hit);
	if (!Frame().contains(point) && hitHandle < 0) {
		Trace("begin_rejected", "outside_frame");
		return;
	}
	// A collective handle is not a body click, even when it lies in empty
	// space. Ctrl still held from selecting layers must not toggle a token off.
	const bool collectiveHandle = hitHandle >= 0 && selectedItems(scope.owner).size() > 1;
	const bool toggle = (modifiers & Qt::ControlModifier) && !collectiveHandle && hitHandle != rotationHandleIndex;
	const QString ownerUuid = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(scope.owner)));
	select(ownerUuid, hit ? obs_sceneitem_get_id(hit) : -1, toggle, hit && obs_sceneitem_selected(hit));
	if (!hit || !obs_sceneitem_selected(hit)) {
		Trace("begin_rejected", "no_hit_or_selection", hit);
		Refresh();
		return;
	}
	item = hit;
	handle = hitHandle;
	rotating = handle == rotationHandleIndex;
	root = OBSGetWeakRef(obs_scene_get_source(scene()));
	gestureOwner = ownerUuid;
	gestureAncestors = scope.ancestors;
	ancestorStates.clear();
	for (const auto &ancestor : gestureAncestors) {
		AncestorState state;
		obs_sceneitem_get_info2(ancestor, &state.info);
		obs_sceneitem_get_crop(ancestor, &state.crop);
		ancestorStates.push_back(state);
	}
	branch = gestureAncestors.empty() ? nullptr : gestureAncestors.front();
	parentToCanvas = scope.toCanvas;
	obs_sceneitem_get_info2(item, &initial);
	obs_sceneitem_get_crop(item, &initialCrop);
	box = worldBox(item, parentToCanvas);
	if (rotating) {
		matrix4 local;
		obs_sceneitem_get_box_transform(item, &local);
		rotationCenter = map(local, {.5, .5});
		const QPointF radial = map(canvasToParent, canvas) - rotationCenter;
		rotationLastAngle = std::atan2(radial.y(), radial.x());
		rotationAccumulated = 0;
	}
	if (!matrix4_inv(&inverse, &box)) {
		Trace("begin_rejected", "singular_box");
		item = nullptr;
		return;
	}
	sourceSize = {int(obs_source_get_width(obs_sceneitem_get_source(item))),
		      int(obs_source_get_height(obs_sceneitem_get_source(item)))};
	if (sourceSize.isEmpty()) {
		Trace("begin_rejected", "empty_source");
		item = nullptr;
		return;
	}
	participants.clear();
	for (auto &layer : items(scope.owner)) {
		if (obs_sceneitem_selected(layer) && editable(layer)) {
			Participant entry;
			entry.item = layer;
			obs_sceneitem_get_info2(layer, &entry.initial);
			obs_sceneitem_get_crop(layer, &entry.crop);
			entry.box = worldBox(layer, parentToCanvas);
			entry.size = QSize(obs_source_get_width(obs_sceneitem_get_source(layer)),
					   obs_source_get_height(obs_sceneitem_get_source(layer)));
			participants.emplace_back(std::move(entry));
		}
	}
	selectionBox = bounds(selectedItems(scope.owner), parentToCanvas);
	Feedback({});
	start = canvas;
	cropping = handle >= 0 && !rotating && (cropMode || (modifiers & Qt::AltModifier));
	before = Snapshot();
	if (before.empty()) {
		Trace("begin_rejected", "empty_snapshot");
		participants.clear();
		gestureAncestors.clear();
		ancestorStates.clear();
		gestureOwner.clear();
		branch = nullptr;
		item = nullptr;
		root = nullptr;
		rotating = false;
		return;
	}
	frozenGroups = std::make_unique<FreezePortraitGroups>(gestureAncestors);
	active = true;
	moved = false;
	display->setFocus(Qt::MouseFocusReason);
	Trace("begin_active");
	// QWidget/QPA supplies the implicit press grab, including on Wayland.
	Refresh();
}

bool PortraitPreviewEditor::ValidGesture() const
{
	invalidReason.clear();
	auto reject = [this](const char *reason) {
		invalidReason = QString::fromUtf8(reason);
		return false;
	};
	OBSSource strong = OBSGetStrongRef(root);
	if (!active || !strong || obs_source_removed(strong) || obs_scene_from_source(strong) != scene().Get() ||
	    OBSBasic::Get()->property("studioEditingCanvas").toString() != "portrait") {
		return reject("root_context");
	}
	const Scope scope = scopeFor(scene(), gestureOwner);
	if (PortraitGroupsSharedWithMain(scene(), scope.ancestors)) {
		return reject("shared_group");
	}
	if (!scope.owner || !scope.enabled || scope.ancestors.size() != gestureAncestors.size()) {
		return reject("owner_or_ancestor_visibility_lock");
	}
	for (size_t i = 0; i < gestureAncestors.size(); ++i) {
		if (scope.ancestors[i] != gestureAncestors[i]) {
			return reject("ancestor_identity");
		}
		obs_transform_info info{};
		obs_sceneitem_crop crop{};
		obs_sceneitem_get_info2(scope.ancestors[i], &info);
		obs_sceneitem_get_crop(scope.ancestors[i], &crop);
		if (!sameTransform(info, ancestorStates[i].info) || !sameCrop(crop, ancestorStates[i].crop)) {
			return reject("ancestor_info_crop");
		}
	}
	// An external ancestor transform invalidates the captured coordinate frame.
	for (QPointF p : {QPointF(0, 0), QPointF(1, 0), QPointF(0, 1)}) {
		if (QLineF(map(scope.toCanvas, p), map(parentToCanvas, p)).length() > .001) {
			return reject("ancestor_world_frame");
		}
	}
	if (!gestureAncestors.empty()) {
		const auto roots = items(scene());
		if (std::any_of(roots.begin(), roots.end(), [](const OBSSceneItem &layer) {
			    return editable(layer) && obs_sceneitem_selected(layer);
		    })) {
			return reject("root_selection_changed");
		}
	}
	const auto layers = items(scope.owner);
	const auto selected = std::count_if(layers.begin(), layers.end(), [](const OBSSceneItem &layer) {
		return editable(layer) && obs_sceneitem_selected(layer);
	});
	if (size_t(selected) != participants.size()) {
		return reject("participant_count");
	}
	for (const auto &entry : participants) {
		auto *source = obs_sceneitem_get_source(entry.item);
		if (obs_sceneitem_get_scene(entry.item) != scope.owner.Get()) {
			return reject("participant_owner");
		}
		if (!editable(entry.item) || !obs_sceneitem_selected(entry.item)) {
			return reject("participant_visibility_lock_selection");
		}
		if (entry.size != QSize(obs_source_get_width(source), obs_source_get_height(source))) {
			return reject("participant_source_dimensions");
		}
	}
	return true;
}

std::string PortraitPreviewEditor::Snapshot() const
{
	if (branch) {
		return CapturePortraitGroupTransforms(scene(), branch);
	}
	// Only the participants' root items belong to this gesture. Selected locked
	// layers and group children must not become collateral changes on Undo.
	OBSDataAutoRelease state = obs_scene_save_transform_states(scene(), false);
	OBSDataArrayAutoRelease scenes = obs_data_get_array(state, "scenes_and_groups");
	OBSDataAutoRelease entry = obs_data_array_item(scenes, obs_data_array_count(scenes) - 1);
	OBSDataArrayAutoRelease savedItems = obs_data_get_array(entry, "items");
	OBSDataArrayAutoRelease filtered = obs_data_array_create();
	for (size_t i = 0; i < obs_data_array_count(savedItems); ++i) {
		OBSDataAutoRelease saved = obs_data_array_item(savedItems, i);
		const auto id = obs_data_get_int(saved, "id");
		if (std::any_of(participants.begin(), participants.end(),
				[id](const Participant &p) { return obs_sceneitem_get_id(p.item) == id; })) {
			obs_data_array_push_back(filtered, saved);
		}
	}
	obs_data_set_array(entry, "items", filtered);
	OBSDataArrayAutoRelease roots = obs_data_array_create();
	obs_data_array_push_back(roots, entry);
	obs_data_set_array(state, "scenes_and_groups", roots);
	return obs_data_get_json(state);
}

void PortraitPreviewEditor::Move(QPointF point, Qt::KeyboardModifiers modifiers)
{
	if (!ValidGesture()) {
		Trace("move_invalid", invalidReason);
		Cancel();
		return;
	}
	QPointF current = CanvasPoint(point);
	// A click does not alter transforms or consume Undo history.
	if (!moved && std::hypot((current - start).x(), (current - start).y()) * Frame().width() / width < 2) {
		return;
	}
	moved = true;
	obs_transform_info info = initial;
	obs_sceneitem_crop crop = initialCrop;
	if (rotating) {
		const QPointF radial = map(canvasToParent, current) - rotationCenter;
		if (std::hypot(radial.x(), radial.y()) < 1e-6) {
			return;
		}
		const double angle = std::atan2(radial.y(), radial.x());
		rotationAccumulated +=
			std::atan2(std::sin(angle - rotationLastAngle), std::cos(angle - rotationLastAngle));
		rotationLastAngle = angle;
		double degrees = double(initial.rot) + rotationAccumulated * 180 / pi;
		if (modifiers & Qt::ShiftModifier) {
			degrees = std::round(degrees / 15) * 15;
		}
		if (!std::isfinite(degrees) || std::abs(degrees) > std::numeric_limits<float>::max()) {
			RestoreParticipants();
			Feedback("invalid_geometry");
			Refresh();
			return;
		}
		Feedback({});
		info.rot = float(degrees);
		bool applied = true;
		ApplyStudioSceneEdit(scene(), [this, &info, &applied](obs_scene_t *) {
			obs_sceneitem_set_info2(item, &info);
			obs_sceneitem_force_update_transform(item);
			matrix4 local;
			obs_sceneitem_get_box_transform(item, &local);
			const QPointF correction = rotationCenter - map(local, {.5, .5});
			const double x = double(info.pos.x) + correction.x();
			const double y = double(info.pos.y) + correction.y();
			if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() ||
			    std::abs(y) > std::numeric_limits<float>::max()) {
				applied = false;
				RestoreParticipants();
				return;
			}
			info.pos.x = float(x);
			info.pos.y = float(y);
			obs_sceneitem_set_info2(item, &info);
			obs_sceneitem_force_update_transform(item);
		});
		if (!applied) {
			Feedback("invalid_geometry");
		}
	} else if (handle < 0) {
		QPointF delta = current - start;
		if (modifiers & Qt::ShiftModifier) {
			if (std::abs(delta.x()) >= std::abs(delta.y())) {
				delta.setY(0);
			} else {
				delta.setX(0);
			}
		}
		delta = vectorMap(canvasToParent, delta);
		ApplyStudioSceneEdit(scene(), [this, delta](obs_scene_t *) {
			for (const auto &entry : participants) {
				auto transformed = entry.initial;
				transformed.pos.x += delta.x();
				transformed.pos.y += delta.y();
				obs_sceneitem_set_info2(entry.item, &transformed);
				obs_sceneitem_force_update_transform(entry.item);
			}
		});
	} else if (participants.size() > 1) {
		using namespace PortraitTransformGeometry;
		std::vector<Input> inputs;
		for (const auto &entry : participants) {
			inputs.push_back({entry.initial, entry.crop, entry.box, entry.size});
		}
		auto result = Transform(inputs, selectionBox, handle, current - start,
					cropping ? Operation::Crop : Operation::Resize,
					!(modifiers & Qt::ShiftModifier));
		if (result.status != Status::Ok) {
			RestoreParticipants();
			Feedback(result.status == Status::UnsupportedShear ? "unsupported_shear" : "invalid_geometry");
			Refresh();
			return;
		}
		Feedback({});
		bool applied = true;
		ApplyStudioSceneEdit(scene(), [this, &result, &applied](obs_scene_t *) {
			for (size_t i = 0; i < participants.size(); ++i) {
				auto &entry = participants[i];
				auto &change = result.changes[i];
				obs_sceneitem_defer_update_begin(entry.item);
				obs_sceneitem_set_info2(entry.item, &change.info);
				obs_sceneitem_set_crop(entry.item, &change.crop);
				obs_sceneitem_defer_update_end(entry.item);
				obs_sceneitem_force_update_transform(entry.item);
				if (change.correctOrigin) {
					const matrix4 updated = worldBox(entry.item, parentToCanvas);
					const QPointF correction = vectorMap(
						canvasToParent, change.desiredBoxOrigin - map(updated, {0, 0}));
					const double x = change.info.pos.x + correction.x();
					const double y = change.info.pos.y + correction.y();
					if (!std::isfinite(x) || !std::isfinite(y) ||
					    std::abs(x) > std::numeric_limits<float>::max() ||
					    std::abs(y) > std::numeric_limits<float>::max()) {
						applied = false;
						break;
					}
					change.info.pos.x = float(x);
					change.info.pos.y = float(y);
					obs_sceneitem_set_info2(entry.item, &change.info);
					obs_sceneitem_force_update_transform(entry.item);
				}
			}
			if (!applied) {
				// Roll back while the same atomic graphics/scene transaction is held.
				for (const auto &entry : participants) {
					obs_sceneitem_defer_update_begin(entry.item);
					obs_sceneitem_set_info2(entry.item, &entry.initial);
					obs_sceneitem_set_crop(entry.item, &entry.crop);
					obs_sceneitem_defer_update_end(entry.item);
					obs_sceneitem_force_update_transform(entry.item);
				}
			}
		});
		if (!applied) {
			Feedback("invalid_geometry");
		}
	} else {
		ApplyStudioSceneEdit(scene(), [this, current, modifiers, &info, &crop](obs_scene_t *) {
			QPointF delta = map(inverse, current) - map(inverse, start);
			const QPointF h = handles[handle];
			double left = 0, top = 0, right = 1, bottom = 1;
			if (cropping) {
				const int w = sourceSize.width() - initialCrop.left - initialCrop.right;
				const int hgt = sourceSize.height() - initialCrop.top - initialCrop.bottom;
				if (w < 2 || hgt < 2) {
					return;
				}
				if (h.x() == 0) {
					crop.left = std::clamp(initialCrop.left + int(std::round(delta.x() * w)), 0,
							       sourceSize.width() - initialCrop.right - 2);
				}
				if (h.x() == 1) {
					crop.right = std::clamp(initialCrop.right - int(std::round(delta.x() * w)), 0,
								sourceSize.width() - initialCrop.left - 2);
				}
				if (h.y() == 0) {
					crop.top = std::clamp(initialCrop.top + int(std::round(delta.y() * hgt)), 0,
							      sourceSize.height() - initialCrop.bottom - 2);
				}
				if (h.y() == 1) {
					crop.bottom = std::clamp(initialCrop.bottom - int(std::round(delta.y() * hgt)),
								 0, sourceSize.height() - initialCrop.top - 2);
				}
				left = double(crop.left - initialCrop.left) / w;
				top = double(crop.top - initialCrop.top) / hgt;
			} else {
				const double minX = 2 / std::max(2.f, std::hypot(box.x.x, box.x.y));
				const double minY = 2 / std::max(2.f, std::hypot(box.y.x, box.y.y));
				if (h.x() == 0) {
					left = std::min(delta.x(), 1 - minX);
				}
				if (h.x() == 1) {
					right = std::max(1 + delta.x(), minX);
				}
				if (h.y() == 0) {
					top = std::min(delta.y(), 1 - minY);
				}
				if (h.y() == 1) {
					bottom = std::max(1 + delta.y(), minY);
				}
				double fx = right - left, fy = bottom - top;
				if (!(modifiers & Qt::ShiftModifier)) {
					double factor = h.x() == .5                            ? fy
							: h.y() == .5                          ? fx
							: std::abs(fx - 1) >= std::abs(fy - 1) ? fx
											       : fy;
					factor = std::max(factor, std::max(minX, minY));
					fx = fy = factor;
					left = h.x() == 0 ? 1 - fx : h.x() == .5 ? (1 - fx) / 2 : 0;
					top = h.y() == 0 ? 1 - fy : h.y() == .5 ? (1 - fy) / 2 : 0;
				}
				if (info.bounds_type == OBS_BOUNDS_NONE) {
					info.scale.x *= fx;
					info.scale.y *= fy;
				} else {
					info.bounds.x *= fx;
					info.bounds.y *= fy;
				}
			}
			obs_sceneitem_defer_update_begin(item);
			obs_sceneitem_set_info2(item, &info);
			obs_sceneitem_set_crop(item, &crop);
			obs_sceneitem_defer_update_end(item);
			obs_sceneitem_force_update_transform(item);
			// Fix the new box origin to the desired point of the old oriented box.
			// This handles rotation, negative scale and all position alignments.
			if (!cropping || info.bounds_type == OBS_BOUNDS_NONE) {
				const matrix4 updated = worldBox(item, parentToCanvas);
				QPointF offset =
					vectorMap(canvasToParent, map(box, {left, top}) - map(updated, {0, 0}));
				info.pos.x += offset.x();
				info.pos.y += offset.y();
				obs_sceneitem_set_info2(item, &info);
				obs_sceneitem_force_update_transform(item);
			}
		});
	}
	Trace("move_applied",
	      QStringLiteral("canvas_delta=%1,%2").arg((current - start).x()).arg((current - start).y()));
	Refresh();
}

void PortraitPreviewEditor::Trace(const char *phase, const QString &detail, obs_sceneitem_t *target) const
{
	if (!qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_GROUPS_ROOT")) {
		return;
	}
	QJsonArray members;
	for (const auto &entry : participants) {
		auto object = traceItem(entry.item);
		object["initial_source_size"] = QJsonArray{entry.size.width(), entry.size.height()};
		members.push_back(object);
	}
	const QRectF frame = Frame();
	OBSScene current = scene();
	const QJsonObject result{
		{"phase", QString::fromUtf8(phase)},
		{"detail", detail},
		{"active", active},
		{"moved", moved},
		{"handle", handle},
		{"crop", cropping},
		{"rotate", rotating},
		{"scope", groupScope},
		{"gesture_owner", gestureOwner},
		{"root", current ? QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(current))) : QString{}},
		{"editing_canvas", OBSBasic::Get()->property("studioEditingCanvas").toString()},
		{"invalid_reason", invalidReason},
		{"display_visible", display->isVisible()},
		{"display_focus", display->hasFocus()},
		{"display_size", QJsonArray{display->width(), display->height()}},
		{"frame", QJsonArray{frame.x(), frame.y(), frame.width(), frame.height()}},
		{"device_pixel_ratio", display->devicePixelRatioF()},
		{"item", traceItem(target ? target : item.Get())},
		{"participants", members},
	};
	display->setProperty("studioPortraitGestureTrace", result.toVariantMap());
	const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
	blog(LOG_INFO, "[portrait-gesture-trace] %s", json.constData());
}

void PortraitPreviewEditor::Feedback(const QString &status)
{
	if (feedbackStatus == status) {
		return;
	}
	feedbackStatus = status;
	display->setProperty("studioPortraitGestureStatus", status);
	if (status.isEmpty()) {
		QToolTip::hideText();
		display->setToolTip(QTStr("Studio.Portrait.Gestures"));
	} else {
		const QString text = QTStr(status == "shared_group"        ? "Studio.Portrait.SharedGroup"
					   : status == "unsupported_shear" ? "Studio.Portrait.UnsupportedShear"
									   : "Studio.Portrait.InvalidGeometry");
		display->setToolTip(text);
		QToolTip::showText(display->mapToGlobal(display->rect().center()), text, display);
	}
}

void PortraitPreviewEditor::RestoreParticipants()
{
	OBSSource strong = OBSGetStrongRef(root);
	if (!strong || obs_source_removed(strong)) {
		return;
	}
	ApplyStudioSceneEdit(obs_scene_from_source(strong), [this](obs_scene_t *target) {
		const Scope scope = scopeFor(target, gestureOwner);
		for (const auto &entry : participants) {
			if (!scope.owner || obs_sceneitem_get_scene(entry.item) != scope.owner.Get()) {
				continue;
			}
			obs_sceneitem_defer_update_begin(entry.item);
			obs_sceneitem_set_info2(entry.item, &entry.initial);
			obs_sceneitem_set_crop(entry.item, &entry.crop);
			obs_sceneitem_defer_update_end(entry.item);
			obs_sceneitem_force_update_transform(entry.item);
		}
	});
}

void PortraitPreviewEditor::Cancel()
{
	if (!active) {
		return;
	}
	Trace("cancel", invalidReason);
	active = false;
	// Never resurrect a removed item or overwrite the next scene's layout.
	OBSSource strong = OBSGetStrongRef(root);
	if (branch && strong && !obs_source_removed(strong)) {
		RestorePortraitGroupTransforms(before, frozenGroups.get());
	} else {
		RestoreParticipants();
	}
	Trace("cancel_restored");
	frozenGroups.reset();
	Feedback({});
	participants.clear();
	gestureAncestors.clear();
	ancestorStates.clear();
	gestureOwner.clear();
	branch = nullptr;
	item = nullptr;
	root = nullptr;
	before.clear();
	rotating = false;
	if (QWidget::mouseGrabber() == display) {
		display->releaseMouse();
	}
	display->unsetCursor();
}

void PortraitPreviewEditor::Finish()
{
	if (!ValidGesture() || !feedbackStatus.isEmpty()) {
		Trace("finish_rejected", invalidReason + ":" + feedbackStatus);
		Cancel();
		Refresh();
		return;
	}
	Trace("finish_before_settle");
	OBSSource strong = OBSGetStrongRef(root);
	// Autosize changes sibling positions and the group's dimensions. Freeze
	// ends before capturing the final branch, under the same graphics lock.
	active = false;
	std::string after;
	ApplyStudioSceneEdit(obs_scene_from_source(strong), [this, &after](obs_scene_t *target) {
		if (frozenGroups) {
			frozenGroups->ReleaseAndSettle(target);
		}
		after = Snapshot();
	});
	Trace("finish_after_settle",
	      QStringLiteral("before_bytes=%1 after_bytes=%2").arg(before.size()).arg(after.size()));
	frozenGroups.reset();
	const std::string uuid = obs_source_get_uuid(strong);
	auto *main = OBSBasic::Get();
	if (moved && !before.empty() && !after.empty() && before != after) {
		const bool groupEdit = bool(branch);
		const QString owner = gestureOwner;
		QPointer<PortraitPreviewEditor> guard(this);
		auto apply = [uuid, groupEdit, owner, guard](const std::string &json) {
			OBSSourceAutoRelease target = obs_get_source_by_uuid(uuid.c_str());
			if (!target || obs_source_removed(target)) {
				return;
			}
			if (guard) {
				guard->Cancel();
			}
			OBSBasic::Get()->SetStudioEditingScene(target);
			if (groupEdit) {
				RestorePortraitGroupTransforms(json);
			} else {
				ApplyStudioSceneEdit(obs_scene_from_source(target), [&json](obs_scene_t *) {
					obs_scene_load_transform_states(json.c_str());
				});
			}
			if (guard) {
				guard->SetGroupScope(groupEdit ? owner : QString{});
				guard->Refresh();
			}
			OBSBasic::Get()->SaveProject();
		};
		main->undo_s.add_action(QTStr(rotating     ? "Studio.Portrait.Rotate"
					      : cropping   ? "Studio.Portrait.Crop"
					      : handle < 0 ? "Studio.Portrait.Move"
							   : "Studio.Portrait.Resize"),
					apply, apply, before, after);
		main->SaveProject();
	}
	active = false;
	participants.clear();
	gestureAncestors.clear();
	ancestorStates.clear();
	gestureOwner.clear();
	branch = nullptr;
	item = nullptr;
	root = nullptr;
	before.clear();
	rotating = false;
	if (QWidget::mouseGrabber() == display) {
		display->releaseMouse();
	}
	Refresh();
}

void PortraitPreviewEditor::Refresh()
{
	if (active && !ValidGesture()) {
		Trace("refresh_invalid", invalidReason);
		Cancel();
	}
	AdoptSelection();
	Overlay updated;
	updated.pixelRatio = display->devicePixelRatioF();
	updated.rejected = !feedbackStatus.isEmpty();
	OBSScene current = scene();
	const Scope scope = scopeFor(current, groupScope);
	if (scope.owner && scope.enabled && OBSBasic::Get()->property("studioEditingCanvas").toString() == "portrait") {
		for (auto &layer : items(scope.owner)) {
			if (!editable(layer) || !obs_sceneitem_selected(layer)) {
				continue;
			}
			matrix4 inv;
			const matrix4 transform = worldBox(layer, scope.toCanvas);
			if (!matrix4_inv(&inv, &transform)) {
				continue;
			}
			std::array<QPointF, 8> points;
			for (size_t i = 0; i < handles.size(); ++i) {
				points[i] = map(transform, handles[i]);
			}
			obs_sceneitem_crop crop;
			obs_sceneitem_get_crop(layer, &crop);
			updated.cropped = updated.cropped || cropMode || crop.left || crop.right || crop.top ||
					  crop.bottom;
			updated.boxes.push_back(points);
		}
	}
	if (updated.boxes.size() == 1) {
		updated.controls = updated.boxes.front();
		const auto selected = selectedItems(scope.owner);
		if (selected.size() == 1 && display->isVisible() &&
		    !PortraitGroupsSharedWithMain(current, scope.ancestors)) {
			updated.rotation = rotationGrip(worldBox(selected.front(), scope.toCanvas),
							24 * width / Frame().width(), 7 * width / Frame().width());
		}
	} else if (updated.boxes.size() > 1) {
		const auto frame = bounds(selectedItems(scope.owner), scope.toCanvas);
		if (frame.isValid()) {
			updated.controls = frameHandles(frame);
			updated.boxes.push_back(*updated.controls);
		}
	}
	if (updated.rotation) {
		display->setProperty("studioPortraitRotationHandle", QVariant::fromValue((*updated.rotation)[1]));
		display->setProperty("studioPortraitRotationCenter", QVariant::fromValue((*updated.rotation)[2]));
	} else {
		display->setProperty("studioPortraitRotationHandle", QVariant{});
		display->setProperty("studioPortraitRotationCenter", QVariant{});
	}
	std::lock_guard lock(overlayMutex);
	overlay = updated;
}

bool PortraitPreviewEditor::eventFilter(QObject *object, QEvent *event)
{
	if (object != display) {
		return false;
	}
	switch (event->type()) {
	case QEvent::MouseButtonDblClick: {
		auto *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() == Qt::LeftButton && scene()) {
			EnterGroup(mouse->position());
			return true;
		}
		break;
	}
	case QEvent::MouseButtonPress: {
		auto *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() != Qt::LeftButton || !scene()) {
			break;
		}
		Begin(mouse->position(), mouse->modifiers());
		return true;
	}
	case QEvent::MouseMove: {
		auto *mouse = static_cast<QMouseEvent *>(event);
		if (active) {
			Move(mouse->position(), mouse->modifiers());
			return true;
		}
		int h;
		OBSSceneItem hit = scene() ? Hit(CanvasPoint(mouse->position()), h) : nullptr;
		Qt::CursorShape cursor = hit ? Qt::OpenHandCursor : Qt::ArrowCursor;
		if (hit && h == rotationHandleIndex) {
			cursor = Qt::CrossCursor;
		} else if (hit && h >= 0) {
			if (cropMode || (mouse->modifiers() & Qt::AltModifier)) {
				cursor = Qt::CrossCursor;
			} else {
				const Scope scope = scopeFor(scene(), groupScope);
				const matrix4 transform = worldBox(hit, scope.toCanvas);
				const auto selection = selectedItems(scope.owner);
				QPointF direction;
				if (selection.size() > 1) {
					const auto frame = bounds(selection, scope.toCanvas);
					direction = frameHandles(frame)[h] - frame.center();
				} else {
					direction = map(transform, handles[h]) - map(transform, {.5, .5});
				}
				const int angle = (int(std::round(std::atan2(direction.y(), direction.x()) * 4 /
								  3.14159265358979323846)) +
						   8) %
						  4;
				const Qt::CursorShape cursors[] = {Qt::SizeHorCursor, Qt::SizeFDiagCursor,
								   Qt::SizeVerCursor, Qt::SizeBDiagCursor};
				cursor = cursors[angle];
			}
		}
		display->setCursor(cursor);
		break;
	}
	case QEvent::MouseButtonRelease:
		if (active && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
			auto *mouse = static_cast<QMouseEvent *>(event);
			Move(mouse->position(), mouse->modifiers());
			Finish();
			return true;
		}
		break;
	case QEvent::KeyPress:
	case QEvent::ShortcutOverride: {
		auto *key = static_cast<QKeyEvent *>(event);
		if (key->key() == Qt::Key_Escape && (active || !groupScope.isEmpty())) {
			// ShortcutOverride reserves the key; only KeyPress performs an
			// action so one Escape cannot both cancel and exit a scope.
			if (event->type() == QEvent::KeyPress) {
				if (active) {
					Cancel();
				} else {
					ExitGroup();
				}
			}
			event->accept();
			return true;
		}
		if (active) {
			// Keep global Undo from racing a half-finished gesture.
			if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)) {
				event->accept();
				return true;
			}
		}
		break;
	}
	case QEvent::Resize:
	case QEvent::Hide:
	case QEvent::FocusOut:
	case QEvent::UngrabMouse:
		if (active) {
			Trace("event_cancel", QStringLiteral("event_type=%1").arg(int(event->type())));
		}
		Cancel();
		break;
	default:
		break;
	}
	return false;
}

void PortraitPreviewEditor::Draw(float displayScale)
{
	Overlay copy;
	{
		std::lock_guard lock(overlayMutex);
		copy = overlay;
	}
	if (copy.boxes.empty() || displayScale <= 0) {
		return;
	}
	const float radius = 3.5f * copy.pixelRatio / displayScale;
	gs_matrix_push();
	gs_matrix_identity();
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 color;
	vec4_set(&color,
		 copy.rejected  ? 1.f
		 : copy.cropped ? .2f
				: .7f,
		 copy.rejected  ? .2f
		 : copy.cropped ? 1.f
				: .45f,
		 copy.rejected ? .2f : 1.f, 1.f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
	while (gs_effect_loop(effect, "Solid")) {
		for (const auto &points : copy.boxes) {
			for (int i : {0, 2, 4, 6}) {
				const QPointF a = points[i], b = points[(i + 2) % 8];
				const QPointF delta = b - a;
				const double length = std::hypot(delta.x(), delta.y());
				if (length < .001) {
					continue;
				}
				const QPointF normal(-delta.y() / length * radius / 3.5,
						     delta.x() / length * radius / 3.5);
				gs_render_start(true);
				for (QPointF point : {a - normal, a + normal, b - normal, b + normal}) {
					gs_vertex2f(point.x(), point.y());
				}
				gs_render_stop(GS_TRISTRIP);
			}
		}
		if (copy.controls) {
			for (const QPointF &p : *copy.controls) {
				gs_render_start(true);
				gs_vertex2f(p.x() - radius, p.y() - radius);
				gs_vertex2f(p.x() + radius, p.y() - radius);
				gs_vertex2f(p.x() - radius, p.y() + radius);
				gs_vertex2f(p.x() + radius, p.y() + radius);
				gs_render_stop(GS_TRISTRIP);
			}
		}
		if (copy.rotation) {
			const QPointF edge = (*copy.rotation)[0], grip = (*copy.rotation)[1];
			const QPointF delta = grip - edge;
			const double length = std::hypot(delta.x(), delta.y());
			if (length > .001) {
				const QPointF normal(-delta.y() / length * radius / 3.5,
						     delta.x() / length * radius / 3.5);
				const QPointF end = grip - delta * (radius * 1.5 / length);
				gs_render_start(true);
				for (QPointF point : {edge - normal, edge + normal, end - normal, end + normal}) {
					gs_vertex2f(point.x(), point.y());
				}
				gs_render_stop(GS_TRISTRIP);
			}
			gs_render_start(true);
			for (int i = 0; i <= 24; ++i) {
				const double angle = i * 2 * pi / 24;
				const QPointF direction(std::cos(angle), std::sin(angle));
				for (float r : {radius * 1.5f, radius * .95f}) {
					const QPointF point = grip + direction * r;
					gs_vertex2f(point.x(), point.y());
				}
			}
			gs_render_stop(GS_TRISTRIP);
		}
	}
	gs_matrix_pop();
}
