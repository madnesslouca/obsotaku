/* Opt-in native Qt/libobs group-child gestures. Only disposable sources are
 * created. A complete process restart is required for the RESTORE phase.
 * Pointer events are injected in Qt; this does not certify a physical mouse,
 * compositor shortcuts, Windows execution, devices or concurrent outputs. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <graphics/matrix4.h>
#include <util/config-file.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

OBS_DECLARE_MODULE()
namespace {
constexpr const char *sceneName = "Portrait Groups QA";
constexpr const char *groupName = "Portrait Groups QA group";
constexpr const char *nestedName = "Portrait Groups QA nested";
constexpr const char *childName = "Portrait Groups QA child";
constexpr const char *siblingName = "Portrait Groups QA sibling";
constexpr const char *leafName = "Portrait Groups QA leaf";
QString root;
QMainWindow *window = nullptr;
QWidget *display = nullptr;
QDockWidget *dock = nullptr;
QJsonObject saved;
QJsonArray checks;
bool restoring = false;
bool recovering = false;
std::string groupLookupName = groupName;
bool traceUnitInput = false;
QJsonArray unitInputTrace;
int optionsMaximumHeight = QWIDGETSIZE_MAX;
bool optionsAdjusted = false;

template<class T> T *get(QObject *parent, const char *name)
{

	auto *result = parent->findChild<T *>(name);
	if (!result) {
		throw std::runtime_error(name);
	}
	return result;
}
void check(bool pass, const char *label, QJsonObject observed = {})
{
	checks.append(QJsonObject{{"check", label}, {"passed", pass}, {"observed", observed}});
	blog(LOG_INFO, "[portrait-groups-check] %s %s %s", pass ? "PASS" : "FAIL", label,
	     QJsonDocument(observed).toJson(QJsonDocument::Compact).constData());
	if (!pass) {
		throw std::runtime_error(label);
	}
}
void write(const char *name, QJsonObject data)
{
	QFile file(root + "/" + name);
	if (!file.open(QIODevice::WriteOnly)) {
		throw std::runtime_error("private report unavailable");
	}
	file.write(QJsonDocument(data).toJson());
}
void registerTemporary(obs_source_t *source)
{
	check(source != nullptr, "temporary source exists before its cleanup identity is journaled");
	QJsonArray identities = saved.value("temporary_sources").toArray();
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
	for (const auto &entry : identities) {
		if (entry.toObject().value("uuid").toString() == uuid) {
			return;
		}
	}
	identities.append(QJsonObject{{"uuid", uuid}, {"name", obs_source_get_name(source)}});
	saved["temporary_sources"] = identities;
	write("saved.json", saved);
}
bool wait(const std::function<bool()> &condition, int milliseconds = 2600)
{
	for (int elapsed = 0; elapsed < milliseconds; elapsed += 40) {
		if (condition()) {
			return true;
		}
		QTest::qWait(40);
	}
	return condition();
}
void mode(bool multi)
{
	bool accepted = false;
	check(QMetaObject::invokeMethod(window, "SetStudioCanvasMode", Qt::DirectConnection,
					Q_RETURN_ARG(bool, accepted), Q_ARG(bool, multi)) &&
		      accepted,
	      "canvas mode accepts the idle fixture action");
}
OBSScene portrait()
{
	OBSCanvasAutoRelease canvas = obs_get_canvas_by_name("Vertical 9:16");
	OBSSourceAutoRelease source = canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
	return source ? obs_scene_from_source(source) : nullptr;
}
struct Path {
	OBSSceneItem item;
	std::vector<OBSSceneItem> ancestors; // outermost first
};
bool find(obs_scene_t *scene, const char *name, Path &result)
{
	struct Search {
		const char *name;
		Path *result;
		bool found = false;
	} search{name, &result};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
			auto &s = *static_cast<Search *>(opaque);
			if (strcmp(obs_source_get_name(obs_sceneitem_get_source(item)), s.name) == 0) {
				s.result->item = item;
				s.found = true;
				return false;
			}
			if (obs_sceneitem_is_group(item)) {
				s.result->ancestors.emplace_back(item);
				if (find(obs_sceneitem_group_get_scene(item), s.name, *s.result)) {
					s.found = true;
					return false;
				}
				s.result->ancestors.pop_back();
			}
			return true;
		},
		&search);
	return search.found;
}
Path path(const char *name)
{
	Path result;
	OBSScene current = portrait();
	const char *lookup = name == groupName ? groupLookupName.c_str() : name;
	if (!current || !find(current, lookup, result)) {
		throw std::runtime_error(name);
	}
	return result;
}
obs_transform_info info(obs_sceneitem_t *item)
{
	obs_transform_info value{};
	obs_sceneitem_get_info2(item, &value);
	return value;
}
obs_sceneitem_crop crop(obs_sceneitem_t *item)
{
	obs_sceneitem_crop value{};
	obs_sceneitem_get_crop(item, &value);
	return value;
}
QPointF map(const matrix4 &transform, QPointF point)
{
	vec3 before{}, after{};
	vec3_set(&before, point.x(), point.y(), 0);
	vec3_transform(&after, &before, &transform);
	return {after.x, after.y};
}
QPointF point(const Path &p, QPointF unit)
{
	matrix4 transform;
	obs_sceneitem_get_box_transform(p.item, &transform);
	QPointF result = map(transform, unit);
	for (auto it = p.ancestors.rbegin(); it != p.ancestors.rend(); ++it) {
		obs_sceneitem_get_content_transform(*it, &transform);
		result = map(transform, result);
	}
	return result;
}
QJsonObject state(obs_sceneitem_t *item)
{
	const auto i = info(item);
	const auto c = crop(item);
	return {{"x", i.pos.x},
		{"y", i.pos.y},
		{"sx", i.scale.x},
		{"sy", i.scale.y},
		{"rot", i.rot},
		{"alignment", int(i.alignment)},
		{"bounds_type", int(i.bounds_type)},
		{"bounds_alignment", int(i.bounds_alignment)},
		{"crop_to_bounds", i.crop_to_bounds},
		{"bw", i.bounds.x},
		{"bh", i.bounds.y},
		{"left", c.left},
		{"right", c.right},
		{"top", c.top},
		{"bottom", c.bottom}};
}
QJsonObject geometry(const char *name)
{
	const auto p = path(name);
	QJsonArray points;
	for (const QPointF unit : {QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)}) {
		const auto at = point(p, unit);
		points.append(QJsonObject{{"x", at.x()}, {"y", at.y()}});
	}
	return {{"points", points}, {"state", state(p.item)}};
}
bool equal(QJsonValue a, QJsonValue b, double tolerance = .12)
{
	if (a.type() != b.type()) {
		return false;
	}
	if (a.isObject()) {
		const auto x = a.toObject(), y = b.toObject();
		if (x.keys() != y.keys()) {
			return false;
		}
		for (const auto &key : x.keys()) {
			if (!equal(x[key], y[key], tolerance)) {
				return false;
			}
		}
		return true;
	}
	if (a.isArray()) {
		const auto x = a.toArray(), y = b.toArray();
		if (x.size() != y.size()) {
			return false;
		}
		for (int n = 0; n < x.size(); ++n) {
			if (!equal(x[n], y[n], tolerance)) {
				return false;
			}
		}
		return true;
	}
	return a.isDouble() ? std::abs(a.toDouble() - b.toDouble()) <= tolerance : a == b;
}
QJsonObject composition()
{
	QJsonObject result;
	for (const char *name : {groupName, childName, siblingName, nestedName, leafName}) {
		result[name] = geometry(name);
	}
	return result;
}
void checkGroupExtents(const char *name, const char *label)
{
	OBSScene extentRoot = portrait();
	OBSSceneItem extentGroup = path(name).item;
	struct Extents {
		double minX = std::numeric_limits<double>::infinity();
		double minY = std::numeric_limits<double>::infinity();
		double maxX = -std::numeric_limits<double>::infinity();
		double maxY = -std::numeric_limits<double>::infinity();
		QJsonArray children;
	} extents;
	obs_enter_graphics();
	obs_scene_prune_sources(extentRoot);
	obs_scene_enum_items(
		obs_sceneitem_group_get_scene(extentGroup),
		[](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
			auto &extent = *static_cast<Extents *>(opaque);
			matrix4 box;
			obs_sceneitem_get_box_transform(item, &box);
			QJsonArray corners;
			for (const QPointF unit : {QPointF(0, 0), QPointF(1, 0), QPointF(0, 1), QPointF(1, 1)}) {
				const QPointF at = map(box, unit);
				extent.minX = std::min(extent.minX, at.x());
				extent.minY = std::min(extent.minY, at.y());
				extent.maxX = std::max(extent.maxX, at.x());
				extent.maxY = std::max(extent.maxY, at.y());
				corners.append(QJsonObject{{"x", at.x()}, {"y", at.y()}});
			}
			auto *source = obs_sceneitem_get_source(item);
			extent.children.append(QJsonObject{{"item_id", double(obs_sceneitem_get_id(item))},
							   {"source_uuid", obs_source_get_uuid(source)},
							   {"name", obs_source_get_name(source)},
							   {"visible", obs_sceneitem_visible(item)},
							   {"corners", corners}});
			return true; // Hidden children count, matching resize_scene_base.
		},
		&extents);
	const int groupWidth = int(obs_source_get_width(obs_sceneitem_get_source(extentGroup)));
	const int groupHeight = int(obs_source_get_height(obs_sceneitem_get_source(extentGroup)));
	obs_leave_graphics();
	const double expectedWidth = std::ceil(extents.maxX - extents.minX);
	const double expectedHeight = std::ceil(extents.maxY - extents.minY);
	check(!extents.children.isEmpty() && std::isfinite(expectedWidth) && std::isfinite(expectedHeight) &&
		      std::abs(groupWidth - expectedWidth) <= 1 && std::abs(groupHeight - expectedHeight) <= 1,
	      label,
	      {{"group", name},
	       {"width", groupWidth},
	       {"height", groupHeight},
	       {"expected_width", expectedWidth},
	       {"expected_height", expectedHeight},
	       {"min_x", extents.minX},
	       {"min_y", extents.minY},
	       {"max_x", extents.maxX},
	       {"max_y", extents.maxY},
	       {"children", extents.children}});
}
QJsonArray diagnosticItems(obs_scene_t *scene)
{
	QJsonArray result;
	if (!scene) {
		return result;
	}
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *owner, obs_sceneitem_t *item, void *opaque) {
			auto &array = *static_cast<QJsonArray *>(opaque);
			auto *source = obs_sceneitem_get_source(item);
			QJsonObject entry{{"owner_uuid", obs_source_get_uuid(obs_scene_get_source(owner))},
					  {"item_id", double(obs_sceneitem_get_id(item))},
					  {"source_uuid", obs_source_get_uuid(source)},
					  {"name", obs_source_get_name(source)},
					  {"group", obs_sceneitem_is_group(item)},
					  {"width", int(obs_source_get_width(source))},
					  {"height", int(obs_source_get_height(source))},
					  {"transform", state(item)},
					  {"visible", obs_sceneitem_visible(item)},
					  {"selected", obs_sceneitem_selected(item)}};
			if (obs_sceneitem_is_group(item)) {
				entry["children"] = diagnosticItems(obs_sceneitem_group_get_scene(item));
			}
			array.append(entry);
			return true;
		},
		&result);
	return result;
}
QJsonObject historyDiagnostic()
{
	std::vector<OBSSource> scenes;
	obs_enum_scenes(
		[](void *opaque, obs_source_t *source) {
			static_cast<std::vector<OBSSource> *>(opaque)->emplace_back(source);
			return true;
		},
		&scenes);
	QJsonArray mainScenes;
	for (const auto &source : scenes) {
		OBSCanvasAutoRelease canvas = obs_source_get_canvas(source);
		mainScenes.append(QJsonObject{{"name", obs_source_get_name(source)},
					      {"uuid", obs_source_get_uuid(source)},
					      {"canvas_uuid", canvas ? obs_canvas_get_uuid(canvas) : ""},
					      {"removed", obs_source_removed(source)},
					      {"items", diagnosticItems(obs_scene_from_source(source))}});
	}
	const auto group = path(groupName).item;
	auto *groupSource = obs_sceneitem_get_source(group);
	OBSCanvasAutoRelease groupCanvas = obs_source_get_canvas(groupSource);
	OBSScene rootScene = portrait();
	OBSCanvasAutoRelease rootCanvas = obs_source_get_canvas(obs_scene_get_source(rootScene));
	auto *undo = get<QAction>(window, "actionMainUndo"), *redo = get<QAction>(window, "actionMainRedo");
	return {{"composition", composition()},
		{"main_scenes", mainScenes},
		{"undo", QJsonObject{{"enabled", undo->isEnabled()}, {"label", undo->text()}}},
		{"redo", QJsonObject{{"enabled", redo->isEnabled()}, {"label", redo->text()}}},
		{"portrait_uuid", obs_source_get_uuid(obs_scene_get_source(rootScene))},
		{"portrait_canvas_uuid", rootCanvas ? obs_canvas_get_uuid(rootCanvas) : ""},
		{"group_item_id", double(obs_sceneitem_get_id(group))},
		{"group_owner_uuid", obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(group)))},
		{"group_source_uuid", obs_source_get_uuid(groupSource)},
		{"group_canvas_uuid", groupCanvas ? obs_canvas_get_uuid(groupCanvas) : ""},
		{"group_width", int(obs_source_get_width(groupSource))},
		{"group_height", int(obs_source_get_height(groupSource))},
		{"scope", display->property("studioPortraitGroupScope").toString()},
		{"gesture_trace",
		 QJsonObject::fromVariantMap(display->property("studioPortraitGestureTrace").toMap())}};
}
QPointF pixel(QPointF canvas)
{
	const double dpr = display->devicePixelRatioF();
	const int w = qRound(display->width() * dpr), h = qRound(display->height() * dpr);
	const double scale = std::min(w / 1080., h / 1920.);
	const int fw = int(1080 * scale), fh = int(1920 * scale);
	return {((w - fw) / 2 + canvas.x() * fw / 1080.) / dpr, ((h - fh) / 2 + canvas.y() * fh / 1920.) / dpr};
}
void ready()
{
	auto *top = display->window();
	top->show();
	top->raise();
	top->activateWindow();
	QApplication::setActiveWindow(top);
	display->setFocus(Qt::OtherFocusReason);
	if (auto *handle = top->windowHandle()) {
		handle->requestActivate();
	}
	QSize previousDisplay, previousWindow;
	int stable = 0;
	const bool allocated = wait(
		[&] {
			const QSize actualDisplay = display->size(), actualWindow = top->size();
			const bool exposed = top->windowHandle() && top->windowHandle()->isExposed();
			if (actualDisplay == previousDisplay && actualWindow == previousWindow &&
			    display->isVisible() && exposed && display->hasFocus() && actualDisplay.width() >= 180 &&
			    actualDisplay.height() >= 320) {
				++stable;
			} else {
				stable = 0;
			}
			previousDisplay = actualDisplay;
			previousWindow = actualWindow;
			return stable >= 18; // 720 ms stable across Qt turns after activation/configure.
		},
		6500);
	check(allocated, "fixture waits for an exposed focused floating preview with stable geometry before input",
	      {{"display_width", display->width()},
	       {"display_height", display->height()},
	       {"window_width", top->width()},
	       {"window_height", top->height()},
	       {"focused", display->hasFocus()},
	       {"stable_turns", stable}});
}
QJsonObject uiDiagnostic()
{
	auto identity = [](QObject *object) {
		return object ? QJsonObject{{"name", object->objectName()},
					    {"class", object->metaObject()->className()}}
			      : QJsonObject{};
	};
	auto *inspector = window ? window->findChild<QDockWidget *>("studioInspectorDock") : nullptr;
	return {{"focus", identity(QApplication::focusWidget())},
		{"active_window", identity(QApplication::activeWindow())},
		{"display_window", identity(display ? display->window() : nullptr)},
		{"display_focus", display && display->hasFocus()},
		{"display_window_active", display && display->window()->isActiveWindow()},
		{"display_width", display ? display->width() : 0},
		{"display_height", display ? display->height() : 0},
		{"display_visible", display && display->isVisible()},
		{"display_dpr", display ? display->devicePixelRatioF() : 0},
		{"scope", display ? display->property("studioPortraitGroupScope").toString() : QString()},
		{"gesture_status", display ? display->property("studioPortraitGestureStatus").toString() : QString()},
		{"inspector_visible", inspector && inspector->isVisible()},
		{"inspector_floating", inspector && inspector->isFloating()}};
}
class UnitEventTrace : public QObject {
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (traceUnitInput && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
				       event->type() == QEvent::WindowActivate ||
				       event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Show ||
				       event->type() == QEvent::Hide || event->type() == QEvent::Resize)) {
			unitInputTrace.append(QJsonObject{{"phase", "widget_event"},
							  {"event_type", int(event->type())},
							  {"object_name", object->objectName()},
							  {"object_class", object->metaObject()->className()},
							  {"ui", uiDiagnostic()}});
		}
		return false;
	}
};
void mouse(QEvent::Type type, QPointF canvas, Qt::KeyboardModifiers modifiers = {})
{
	if (traceUnitInput) {
		unitInputTrace.append(
			QJsonObject{{"phase", "before_activation"}, {"event_type", int(type)}, {"ui", uiDiagnostic()}});
	}
	if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
		QApplication::setActiveWindow(display->window());
		display->setFocus(Qt::MouseFocusReason);
	}
	const QPointF p = pixel(canvas);
	QMouseEvent event(type, p, p, display->mapToGlobal(p),
			  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
			  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
	if (traceUnitInput) {
		unitInputTrace.append(QJsonObject{{"phase", "before_dispatch"},
						  {"event_type", int(type)},
						  {"canvas_x", canvas.x()},
						  {"canvas_y", canvas.y()},
						  {"pixel_x", p.x()},
						  {"pixel_y", p.y()},
						  {"ui", uiDiagnostic()}});
	}
	QApplication::sendEvent(display, &event);
	if (traceUnitInput) {
		unitInputTrace.append(QJsonObject{{"phase", "after_dispatch"},
						  {"event_type", int(type)},
						  {"ui", uiDiagnostic()},
						  {"composition", composition()}});
	}
	QTest::qWait(45);
	if (traceUnitInput) {
		unitInputTrace.append(QJsonObject{{"phase", "after_event_loop"},
						  {"event_type", int(type)},
						  {"ui", uiDiagnostic()},
						  {"composition", composition()}});
	}
}
void gesture(QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = {})
{
	ready();
	mouse(QEvent::MouseButtonPress, from, modifiers);
	for (int n = 1; n <= 5; ++n) {
		mouse(QEvent::MouseMove, from + (to - from) * n / 5., modifiers);
	}
	mouse(QEvent::MouseButtonRelease, to, modifiers);
	QTest::qWait(200);
}
void treeSelect(const char *name)
{
	get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
	auto *tree = get<QListView>(window, "sources");
	check(wait([&] {
		      for (int row = 0; row < tree->model()->rowCount(); ++row) {
			      const QModelIndex index = tree->model()->index(row, 0);
			      if (index.data(Qt::AccessibleTextRole).toString() == name) {
				      tree->selectionModel()->setCurrentIndex(
					      index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
				      return obs_sceneitem_selected(path(name).item);
			      }
		      }
		      return false;
	      }),
	      "native source tree selects the requested owner-scene child", {{"name", name}});
	check(wait([&] {
		      const auto selected = path(name);
		      const QString expected = selected.ancestors.empty()
						       ? QString()
						       : QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(
								 obs_sceneitem_get_scene(selected.item))));
		      return display->property("studioPortraitGroupScope").toString() == expected;
	      }),
	      "native tree selection changes preview group scope to the owner UUID", {{"name", name}});
	ready();
}
void enterGroup(const char *name)
{
	ready();
	const auto center = point(path(name), {.5, .5});
	mouse(QEvent::MouseButtonDblClick, center);
	mouse(QEvent::MouseButtonRelease, center);
	check(wait([&] {
		      return display->property("studioPortraitGroupScope").toString() ==
			     QString::fromUtf8(obs_source_get_uuid(obs_sceneitem_get_source(path(name).item)));
	      }),
	      "double click changes preview group scope immediately", {{"name", name}});
	ready();
}
void history(const QJsonObject &before, const QJsonObject &after)
{
	check(!equal(before, after), "child gesture changes the backend before checking history");
	get<QAction>(window, "actionMainUndo")->trigger();
	check(wait([&] { return equal(composition(), before, .3); }),
	      "one Undo restores child and complete ancestor geometry",
	      {{"actual", composition()}, {"expected", before}});
	get<QAction>(window, "actionMainRedo")->trigger();
	check(wait([&] { return equal(composition(), after, .3); }),
	      "one Redo reapplies child without disturbing siblings", {{"actual", composition()}, {"expected", after}});
}
void go(obs_source_t *source)
{
	obs_frontend_set_current_scene(source);
	const std::string expected = obs_source_get_uuid(source);
	check(wait([&] {
		      OBSScene current = portrait();
		      if (!current) {
			      return false;
		      }
		      OBSDataAutoRelease data = obs_source_get_private_settings(obs_scene_get_source(current));
		      return expected == obs_data_get_string(data, "otaku_main_scene_uuid");
	      }),
	      "portrait channel follows the requested Main UUID before gestures");
}
void cleanup()
{
	if (saved.isEmpty()) {
		return;
	}
	if (optionsAdjusted) {
		if (auto *options = window->findChild<QScrollArea *>("verticalPreviewOptionsScroll")) {
			options->setMaximumHeight(optionsMaximumHeight);
		}
		optionsAdjusted = false;
	}
	QMetaObject::invokeMethod(window, "SetStudioPortraitEditing", Qt::DirectConnection, Q_ARG(bool, false));
	OBSSourceAutoRelease original = obs_get_source_by_uuid(saved["scene"].toString().toUtf8().constData());
	if (original) {
		obs_frontend_set_current_scene(original);
	}
	if (groupLookupName != groupName) {
		OBSSourceAutoRelease renamed = obs_get_source_by_name(groupLookupName.c_str());
		if (renamed) {
			obs_source_set_name(renamed, groupName);
		}
		groupLookupName = groupName;
	}
	for (const auto &entry : saved.value("temporary_sources").toArray()) {
		const QJsonObject identity = entry.toObject();
		const QString uuid = identity.value("uuid").toString(), name = identity.value("name").toString();
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		check(!source || name == QString::fromUtf8(obs_source_get_name(source)),
		      "temporary cleanup verifies the created UUID and original name before removal",
		      {{"uuid", uuid}, {"name", name}, {"already_absent", !source}});
		if (source) {
			obs_source_remove(source);
		}
	}
	for (const char *name : {sceneName, groupName, nestedName, childName, siblingName, leafName}) {
		OBSSourceAutoRelease source = obs_get_source_by_name(name);
		if (source) {
			obs_source_remove(source);
		}
	}
	mode(saved["multi"].toBool());
	if (!saved["had_multi"].toBool()) {
		config_remove_value(obs_frontend_get_user_config(), "BasicWindow", "StudioMultiCanvas");
	}
	config_save_safe(obs_frontend_get_user_config(), "tmp", nullptr);
	if (saved["studio"].toBool()) {
		obs_frontend_set_preview_program_mode(true);
		OBSSourceAutoRelease preview = obs_get_source_by_uuid(saved["preview"].toString().toUtf8().constData());
		if (preview) {
			obs_frontend_set_current_preview_scene(preview);
		}
	}
	window->restoreState(QByteArray::fromBase64(saved["layout"].toString().toUtf8()));
	obs_frontend_save();
}
void setPose(obs_sceneitem_t *item, QPointF position, double rotation = 0, bool negative = false, bool bounds = false)
{
	obs_transform_info transform{};
	transform.pos = {float(position.x()), float(position.y())};
	transform.scale = {negative ? -1.f : 1.f, 1.f};
	transform.rot = rotation;
	transform.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	transform.bounds_type = bounds ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_NONE;
	transform.bounds = {680, 700};
	transform.bounds_alignment = OBS_ALIGN_CENTER;
	obs_sceneitem_crop zero{};
	obs_sceneitem_set_locked(item, false);
	obs_sceneitem_set_visible(item, true);
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &transform);
	obs_sceneitem_set_crop(item, &zero);
	obs_sceneitem_defer_update_end(item);
}
void fixture()
{
	for (const char *name : {sceneName, groupName, nestedName, childName, siblingName, leafName}) {
		OBSSourceAutoRelease collision = obs_get_source_by_name(name);
		check(!collision, "disposable group namespace is unused", {{"name", name}});
	}
	OBSSourceAutoRelease original = obs_frontend_get_current_scene(),
			     preview = obs_frontend_get_current_preview_scene();
	auto *config = obs_frontend_get_user_config();
	saved = {{"scene", obs_source_get_uuid(original)},
		 {"preview", preview ? obs_source_get_uuid(preview) : ""},
		 {"multi", !config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
				   config_get_bool(config, "BasicWindow", "StudioMultiCanvas")},
		 {"had_multi", config_has_user_value(config, "BasicWindow", "StudioMultiCanvas")},
		 {"studio", obs_frontend_preview_program_mode_active()},
		 {"layout", QString::fromUtf8(window->saveState().toBase64())}};
	write("saved.json", saved);
	obs_frontend_set_preview_program_mode(false);
	mode(true);
	OBSSceneAutoRelease main = obs_scene_create(sceneName);
	for (const char *name : {childName, siblingName, leafName}) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "width", name == childName ? 360 : 120);
		obs_data_set_int(settings, "height", name == childName ? 300 : 150);
		obs_data_set_int(settings, "color", name == childName ? 0xff00aaff : 0xff66dd22);
		OBSSourceAutoRelease source = obs_source_create("color_source_v3", name, settings, nullptr);
		check(bool(source), "disposable child shares a real color source with Main");
		setPose(obs_scene_add(main, source), {30, 50});
	}
	go(obs_scene_get_source(main));
	auto *parent = obs_scene_add_group2(portrait(), groupName, true);
	auto *owner = obs_sceneitem_group_get_scene(parent);
	for (const char *name : {childName, siblingName}) {
		OBSSourceAutoRelease source = obs_get_source_by_name(name);
		setPose(obs_scene_add(owner, source), name == childName ? QPointF(0, 0) : QPointF(420, 420));
	}
	auto *nested = obs_scene_add_group2(owner, nestedName, true);
	OBSSourceAutoRelease leaf = obs_get_source_by_name(leafName);
	setPose(obs_scene_add(obs_sceneitem_group_get_scene(nested), leaf), {0, 0});
	QTest::qWait(250);
	setPose(nested, {520, 60});
	setPose(parent, {140, 420});
	obs_scene_enum_items(
		portrait(),
		[](obs_scene_t *, obs_sceneitem_t *item, void *) {
			OBSDataAutoRelease data = obs_sceneitem_get_private_settings(item);
			if (obs_data_get_bool(data, "otaku_mirror")) {
				obs_sceneitem_set_visible(item, false);
			}
			return true;
		},
		nullptr);
	QTest::qWait(250);
	get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
	QMetaObject::invokeMethod(get<QListView>(window, "sources"), "RefreshItems", Qt::DirectConnection);
}
void run()
{
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active(), "group fixture starts with every encoder idle");
		if (recovering) {
			QFile file(root + "/saved.json");
			check(file.open(QIODevice::ReadOnly), "private baseline exists for fixture-only recovery");
			saved = QJsonDocument::fromJson(file.readAll()).object();
			check(!saved.isEmpty() && !saved["scene"].toString().isEmpty(),
			      "fixture-only recovery baseline identifies the original scene");
			cleanup();
			write("recovery.json",
			      {{"passed", true},
			       {"error", ""},
			       {"checks", checks},
			       {"scope",
				"Fixture-only cleanup from the saved private baseline. No new fixture, input gesture or media output is started."}});
			return;
		}
		if (restoring) {
			QFile file(root + "/saved.json");
			check(file.open(QIODevice::ReadOnly), "private restart baseline exists");
			saved = QJsonDocument::fromJson(file.readAll()).object();
			mode(true);
			OBSSourceAutoRelease main = obs_get_source_by_name(sceneName);
			check(bool(main), "restart retains the disposable Main scene");
			go(main);
		} else {
			fixture();
		}
		dock = get<QDockWidget>(window, "verticalPreviewDock");
		dock->setFloating(true);
		dock->show();
		const QRect available = dock->screen()->availableGeometry();
		dock->resize(std::min(560, available.width() - 60), std::min(1000, available.height() - 60));
		dock->raise();
		display = get<QWidget>(window, "verticalPreviewDisplay");
		auto *options = get<QScrollArea>(window, "verticalPreviewOptionsScroll");
		optionsMaximumHeight = options->maximumHeight();
		options->setMaximumHeight(140);
		optionsAdjusted = true;
		check(wait([] { return display->isVisible() && display->width() >= 180 && display->height() >= 320; }),
		      "native group preview has usable geometry");
		ready();
		OBSSourceAutoRelease main = obs_get_source_by_name(sceneName);
		auto mainState = [&] {
			QJsonObject result;
			for (const char *name : {childName, siblingName, leafName}) {
				result[name] = state(obs_scene_find_source(obs_scene_from_source(main), name));
			}
			return result;
		};
		const auto mainBefore = mainState();
		check(path(childName).ancestors.size() == 1 && path(leafName).ancestors.size() == 2,
		      "fixture contains both direct and nested children");
		if (!restoring) {
			check(obs_sceneitem_get_id(path(childName).item) == obs_sceneitem_get_id(path(leafName).item),
			      "different owner scenes intentionally reuse the same child item ID");
		}
		if (restoring) {
			check(saved["portrait_uuid"].toString() ==
				      obs_source_get_uuid(obs_scene_get_source(portrait())),
			      "restart preserves portrait scene UUID");
			check(equal(composition(), saved["final"], .35),
			      "restart restores nested graph geometry and crop",
			      {{"actual", composition()}, {"expected", saved["final"]}});
			check(equal(mainBefore, saved["main_before"]), "restart retains untouched Main source items");
			for (const char *name : {groupName, nestedName, childName, siblingName, leafName}) {
				const auto item = path(name).item;
				const auto identity = saved[QString(name) + "_identity"].toObject();
				check(identity["item"].toDouble() == obs_sceneitem_get_id(item) &&
					      identity["owner"].toString() == obs_source_get_uuid(obs_scene_get_source(
										      obs_sceneitem_get_scene(item))) &&
					      identity["source"].toString() ==
						      obs_source_get_uuid(obs_sceneitem_get_source(item)),
				      "restart preserves owner UUID plus item ID and shared source identity",
				      {{"name", name}});
			}
			treeSelect(leafName);
			check(crop(path(leafName).item).left > 0, "nested child persists an actual source-pixel crop");
			cleanup();
		} else {
			// Outside a group, its whole box remains the interaction target.
			treeSelect(groupName);
			const auto unitBefore = composition();
			const auto unitUiBefore = uiDiagnostic();
			const QPointF siblingBefore = point(path(siblingName), {0, 0});
			const QPointF center = point(path(childName), {.5, .5});
			UnitEventTrace unitEvents;
			for (QObject *object :
			     {static_cast<QObject *>(display), static_cast<QObject *>(dock),
			      static_cast<QObject *>(window),
			      static_cast<QObject *>(get<QDockWidget>(window, "studioInspectorDock"))}) {
				object->installEventFilter(&unitEvents);
			}
			traceUnitInput = true;
			gesture(center, center + QPointF(32, 41));
			traceUnitInput = false;
			const auto unitAfter = composition();
			const QPointF actualDelta = point(path(siblingName), {0, 0}) - siblingBefore;
			const QJsonObject unitDiagnostic{
				{"before", unitBefore},
				{"after", unitAfter},
				{"ui_before", unitUiBefore},
				{"ui_after", uiDiagnostic()},
				{"actual_delta", QJsonObject{{"x", actualDelta.x()}, {"y", actualDelta.y()}}},
				{"expected_delta", QJsonObject{{"x", 32}, {"y", 41}}},
				{"pointer_start", QJsonObject{{"x", center.x()}, {"y", center.y()}}},
				{"input_trace", unitInputTrace}};
			write("diagnosticunit.json", unitDiagnostic);
			check(QLineF(actualDelta, QPointF(32, 41)).length() < 1,
			      "single click outside group scope moves the group as a unit", unitDiagnostic);
			history(unitBefore, composition());
			get<QAction>(window, "actionMainUndo")->trigger();
			QTest::qWait(150);
			enterGroup(groupName);
			const auto childCenter = point(path(childName), {.5, .5});
			gesture(childCenter, childCenter);
			check(obs_sceneitem_selected(path(childName).item) &&
				      !obs_sceneitem_selected(path(groupName).item),
			      "double click enters group and click selects its child instead of ancestor");
			enterGroup(nestedName);
			const auto leafCenter = point(path(leafName), {.5, .5});
			gesture(leafCenter, leafCenter);
			check(obs_sceneitem_selected(path(leafName).item),
			      "a nested scope resolves the reused child ID inside its own owner scene");
			QTest::keyClick(display, Qt::Key_Escape);
			check(wait([] {
				      return display->property("studioPortraitGroupScope").toString() ==
					     QString::fromUtf8(obs_source_get_uuid(
						     obs_sceneitem_get_source(path(groupName).item)));
			      }),
			      "Esc without a gesture exits one nested group level");
			QTest::keyClick(display, Qt::Key_Escape);
			check(wait([] { return display->property("studioPortraitGroupScope").toString().isEmpty(); }),
			      "a second Esc without a gesture returns to portrait root scope");
			treeSelect(childName);
			const auto pairBefore = composition();
			const auto directCenter = point(path(childName), {.5, .5});
			const auto siblingCenter = point(path(siblingName), {.5, .5});
			gesture(siblingCenter, siblingCenter, Qt::ControlModifier);
			check(obs_sceneitem_selected(path(childName).item) &&
				      obs_sceneitem_selected(path(siblingName).item),
			      "Ctrl selection includes siblings inside one group owner");
			gesture(directCenter, directCenter + QPointF(19, 23));
			check(QLineF(point(path(childName), {.5, .5}), directCenter + QPointF(19, 23)).length() < 1 &&
				      QLineF(point(path(siblingName), {.5, .5}), siblingCenter + QPointF(19, 23))
						      .length() < 1,
			      "a grouped sibling selection moves together in global canvas coordinates");
			history(pairBefore, composition());

			for (int variant = 0; variant < 6; ++variant) {
				setPose(path(groupName).item, variant == 2 ? QPointF(850, 480) : QPointF(160, 430),
					variant == 1 ? 22 : 0, variant == 2, variant == 3);
				if (variant == 2) {
					const vec2 scale{-.8f, 1.3f};
					obs_sceneitem_set_scale(path(groupName).item, &scale);
				} else if (variant >= 4) {
					auto aligned = info(path(groupName).item);
					aligned.alignment = variant == 4 ? OBS_ALIGN_CENTER
									 : OBS_ALIGN_RIGHT | OBS_ALIGN_BOTTOM;
					aligned.pos = variant == 4 ? vec2{420, 900} : vec2{900, 1450};
					obs_sceneitem_set_info2(path(groupName).item, &aligned);
				}
				QTest::qWait(220);
				for (const char *target : {childName, leafName}) {
					treeSelect(target);
					const auto before = composition();
					const QJsonValue siblingBefore = geometry(siblingName).value("points");
					const auto from = point(path(target), {.5, .5});
					const QPointF delta(26, 37);
					gesture(from, from + delta);
					if (variant == 3) {
						const auto parent = info(path(groupName).item);
						check(!equal(geometry(target), before[target]) &&
							      parent.bounds_type == OBS_BOUNDS_SCALE_INNER &&
							      parent.bounds.x == 680 && parent.bounds.y == 700 &&
							      std::isfinite(point(path(target), {.5, .5}).x()) &&
							      std::isfinite(point(path(target), {.5, .5}).y()),
						      "bounded ancestor retains its framing while native autosize refits child content",
						      {{"name", target}, {"actual", geometry(target)}});
					} else {
						check(QLineF(point(path(target), {.5, .5}), from + delta).length() <
							      1.2,
						      "child drag follows canvas pixels through ancestor rotation or negative scale",
						      {{"variant", variant},
						       {"name", target},
						       {"actual", geometry(target)}});
						check(equal(geometry(siblingName).value("points"), siblingBefore, .35),
						      "unbounded autosizing ancestors preserve an unselected sibling in global canvas coordinates",
						      {{"variant", variant},
						       {"target", target},
						       {"before_sibling", siblingBefore},
						       {"after_sibling", geometry(siblingName).value("points")},
						       {"before_composition", before},
						       {"after_composition", composition()}});
					}
					history(before, composition());
				}
			}
			// Resize/crop use the oriented global child box while writing local source state.
			setPose(path(groupName).item, {160, 430}, 18);
			QTest::qWait(200);
			for (const char *target : {childName, leafName}) {
				treeSelect(target);
				auto before = composition();
				auto p = path(target);
				const auto oldInfo = info(p.item);
				const QPointF anchor = point(p, {0, 0}), edge = point(p, {1, 1});
				gesture(edge, anchor + (edge - anchor) * 1.16);
				check(std::abs(info(path(target).item).scale.x - oldInfo.scale.x * 1.16) < .008,
				      "global child handle changes the local child scale", {{"name", target}});
				check(QLineF(point(path(target), {0, 0}), anchor).length() < 1,
				      "child resize keeps its opposite global corner anchored");
				history(before, composition());
				before = composition();
				p = path(target);
				const auto start = point(p, {0, .5}), end = point(p, {.1, .5});
				const auto opposite = point(p, {1, .5});
				get<QToolButton>(window, "studioPortraitCropMode")->setChecked(true);
				gesture(start, end);
				get<QToolButton>(window, "studioPortraitCropMode")->setChecked(false);
				check(crop(path(target).item).left > 0, "child crop modifies local source pixels",
				      {{"name", target}});
				check(QLineF(point(path(target), {1, .5}), opposite).length() < 1,
				      "child crop preserves its opposite global edge through the ancestor chain");
				history(before, composition());
			}
			treeSelect(childName);
			const auto renameBefore = composition();
			const auto renameAt = point(path(childName), {.5, .5});
			gesture(renameAt, renameAt + QPointF(20, 22));
			const auto renameAfter = composition();
			OBSSource renamedGroup = obs_sceneitem_get_source(path(groupName).item);
			groupLookupName = "Portrait Groups QA renamed";
			obs_source_set_name(renamedGroup, groupLookupName.c_str());
			history(renameBefore, renameAfter);
			check(QString::fromUtf8(obs_source_get_name(renamedGroup)) ==
				      QString::fromStdString(groupLookupName),
			      "child history resolves owner UUID and preserves a later group rename");
			obs_source_set_name(renamedGroup, groupName);
			groupLookupName = groupName;
			// Effective ancestor crop changes source-to-canvas mapping, not the
			// child's own crop. The public content transform includes both crop
			// forms and is also used by the fixture's independent world helper.
			obs_sceneitem_crop parentCrop{};
			parentCrop.left = 40;
			parentCrop.top = 35;
			parentCrop.right = 20;
			parentCrop.bottom = 10;
			obs_sceneitem_set_crop(path(groupName).item, &parentCrop);
			QTest::qWait(220);
			for (const char *target : {childName, leafName}) {
				treeSelect(target);
				const auto before = composition();
				const QJsonValue siblingBefore = geometry(siblingName).value("points");
				const auto from = point(path(target), {.5, .5});
				gesture(from, from + QPointF(18, 21));
				check(QLineF(point(path(target), {.5, .5}), from + QPointF(18, 21)).length() < 1.2,
				      "child drag includes the ancestor manual-crop translation", {{"name", target}});
				check(equal(geometry(siblingName).value("points"), siblingBefore, .4) &&
					      crop(path(groupName).item).left == 40 &&
					      crop(path(groupName).item).top == 35,
				      "child gesture preserves ancestor crop and unselected sibling placement");
				history(before, composition());
			}
			setPose(path(groupName).item, {180, 500});
			auto bounded = info(path(groupName).item);
			bounded.bounds_type = OBS_BOUNDS_SCALE_OUTER;
			bounded.bounds = {680, 380};
			bounded.crop_to_bounds = true;
			obs_sceneitem_set_info2(path(groupName).item, &bounded);
			QTest::qWait(220);
			obs_sceneitem_crop effectiveCrop{};
			obs_sceneitem_get_effective_crop(path(groupName).item, &effectiveCrop);
			check(effectiveCrop.top + effectiveCrop.bottom > 0,
			      "native outer bounds create a real effective ancestor crop");
			treeSelect(childName);
			const auto boundedBefore = composition();
			const auto boundedFrom = point(path(childName), {.5, .5});
			gesture(boundedFrom, boundedFrom + QPointF(14, 15));
			check(!equal(composition(), boundedBefore) &&
				      obs_sceneitem_get_bounds_crop(path(groupName).item),
			      "visible child remains editable through ancestor bounds crop with native refit");
			history(boundedBefore, composition());
			// Isolate clipping geometry from all previous resize/crop/rebase gestures.
			const char *clipGroupName = "Portrait Groups QA clipping";
			const char *clipTopName = "Portrait Groups QA clipping top";
			const char *clipBottomName = "Portrait Groups QA clipping bottom";
			for (const char *name : {clipGroupName, clipTopName, clipBottomName}) {
				OBSSourceAutoRelease collision = obs_get_source_by_name(name);
				check(!collision, "isolated clipping namespace is unused");
			}
			OBSSceneItem clipGroup = obs_scene_add_group2(portrait(), clipGroupName, true);
			registerTemporary(obs_sceneitem_get_source(clipGroup));
			for (const char *name : {clipTopName, clipBottomName}) {
				OBSDataAutoRelease settings = obs_data_create();
				obs_data_set_int(settings, "width", 680);
				obs_data_set_int(settings, "height", 100);
				obs_data_set_int(settings, "color", 0xffbb8844);
				OBSSourceAutoRelease source =
					obs_source_create("color_source_v3", name, settings, nullptr);
				check(bool(source), "isolated clipping source is created with known dimensions");
				registerTemporary(source);
				setPose(obs_scene_add(obs_sceneitem_group_get_scene(clipGroup), source),
					name == clipTopName ? QPointF(0, 0) : QPointF(0, 1400));
			}
			QTest::qWait(180);
			setPose(clipGroup, {180, 500});
			auto clipInfo = info(clipGroup);
			clipInfo.bounds_type = OBS_BOUNDS_SCALE_OUTER;
			clipInfo.bounds = {680, 380};
			clipInfo.crop_to_bounds = true;
			obs_sceneitem_set_info2(clipGroup, &clipInfo);
			QTest::qWait(220);
			QMetaObject::invokeMethod(get<QListView>(window, "sources"), "RefreshItems",
						  Qt::DirectConnection);
			treeSelect(clipBottomName);
			auto clipState = [&] {
				return QJsonObject{{"principal", composition()},
						   {"parent", geometry(clipGroupName)},
						   {"top", geometry(clipTopName)},
						   {"bottom", geometry(clipBottomName)}};
			};
			const auto clippedBefore = clipState();
			const QPointF clippedUnit(.5, .5);
			const auto clippedAt = point(path(clipBottomName), clippedUnit);
			matrix4 siblingBox;
			obs_sceneitem_get_box_transform(path(clipBottomName).item, &siblingBox);
			const QPointF clippedLocal = map(siblingBox, clippedUnit);
			obs_sceneitem_get_effective_crop(clipGroup, &effectiveCrop);
			auto *parentSource = obs_sceneitem_get_source(clipGroup);
			const int parentWidth = int(obs_source_get_width(parentSource)),
				  parentHeight = int(obs_source_get_height(parentSource));
			const QPointF clippedPixel = pixel(clippedAt);
			const QJsonObject clippingObserved{
				{"crop", QJsonObject{{"left", effectiveCrop.left},
						     {"top", effectiveCrop.top},
						     {"right", effectiveCrop.right},
						     {"bottom", effectiveCrop.bottom}}},
				{"parent_width", parentWidth},
				{"parent_height", parentHeight},
				{"parent_state", state(clipGroup)},
				{"unit", QJsonObject{{"x", clippedUnit.x()}, {"y", clippedUnit.y()}}},
				{"local", QJsonObject{{"x", clippedLocal.x()}, {"y", clippedLocal.y()}}},
				{"world", QJsonObject{{"x", clippedAt.x()}, {"y", clippedAt.y()}}},
				{"pixel", QJsonObject{{"x", clippedPixel.x()}, {"y", clippedPixel.y()}}},
				{"preview_width", display->width()},
				{"preview_height", display->height()},
				{"dpr", display->devicePixelRatioF()}};
			check(clippedLocal.y() < effectiveCrop.top ||
				      clippedLocal.y() > parentHeight - effectiveCrop.bottom ||
				      clippedLocal.x() < effectiveCrop.left ||
				      clippedLocal.x() > parentWidth - effectiveCrop.right,
			      "clipping fixture interior point is independently outside the effective ancestor source rectangle",
			      clippingObserved);
			check(QRectF(0, 0, 1080, 1920).contains(clippedAt) &&
				      QRectF(0, 0, 1080, 1920).contains(clippedAt + QPointF(18, 19)) &&
				      display->rect().contains(clippedPixel.toPoint()),
			      "clipping fixture pointer is inside the canvas and preview viewport rather than outside the widget",
			      clippingObserved);
			const auto clippedUndo = get<QAction>(window, "actionMainUndo")->text();
			gesture(clippedAt, clippedAt + QPointF(18, 19));
			check(equal(clippedBefore, clipState(), .4) &&
				      get<QAction>(window, "actionMainUndo")->text() == clippedUndo,
			      "ancestor bounds clipping rejects pointer hits on clipped child content without history",
			      clippingObserved);
			obs_sceneitem_remove(clipGroup);
			for (const char *name : {clipGroupName, clipTopName, clipBottomName}) {
				OBSSourceAutoRelease source = obs_get_source_by_name(name);
				if (source) {
					obs_source_remove(source);
				}
			}
			QTest::qWait(180);
			setPose(path(groupName).item, {160, 430}, 18);
			QTest::qWait(220);
			for (const char *target : {childName, leafName}) {
				for (const char *field : {"studioPositionX", "studioWidth", "studioRotation"}) {
					treeSelect(target);
					auto *spin = get<QDoubleSpinBox>(window, field);
					check(wait([&] {
						      return get<QLabel>(window, "studioSelectedSourceName")->text() ==
								     target &&
							     spin->isEnabled();
					      }),
					      "numeric inspector binds an editable group child",
					      {{"name", target}, {"field", field}});
					const auto before = composition();
					const QJsonValue siblingBefore = geometry(siblingName).value("points");
					const double desired =
						spin->value() + (QString(field) == "studioRotation" ? 8 : 20);
					spin->setValue(desired);
					check(QMetaObject::invokeMethod(spin, "editingFinished", Qt::DirectConnection),
					      "numeric inspector commits through the real Qt editing signal");
					check(wait([&] { return !equal(composition(), before); }),
					      "numeric child edit updates the effective backend immediately",
					      {{"name", target}, {"field", field}});
					if (QString(field) == "studioRotation") {
						check(std::abs(info(path(target).item).rot - desired) < .05,
						      "numeric child rotation reaches the requested local angle");
					} else if (QString(field) == "studioWidth") {
						const auto item = path(target).item;
						const auto currentCrop = crop(item);
						const double width =
							std::abs(info(item).scale.x) *
							(obs_source_get_width(obs_sceneitem_get_source(item)) -
							 currentCrop.left - currentCrop.right);
						check(std::abs(width - desired) < .15,
						      "numeric child width retains source-pixel crop semantics");
					}
					check(equal(geometry(siblingName).value("points"), siblingBefore, .4),
					      "numeric child edit preserves an unselected sibling globally");
					if (target == leafName && QString(field) == "studioRotation") {
						const auto after = composition();
						OBSSource numericGroup = obs_sceneitem_get_source(path(groupName).item);
						groupLookupName = "Portrait Groups QA numeric renamed";
						obs_source_set_name(numericGroup, groupLookupName.c_str());
						history(before, after);
						check(QString::fromUtf8(obs_source_get_name(numericGroup)) ==
							      QString::fromStdString(groupLookupName),
						      "numeric child Undo resolves UUID after a later ancestor rename");
						obs_source_set_name(numericGroup, groupName);
						groupLookupName = groupName;
					} else {
						history(before, composition());
					}
				}
			}

			// Ancestral eligibility and interruption are checked during a live gesture.
			for (int interruption = 0; interruption < 4; ++interruption) {
				treeSelect(leafName);
				const auto before = composition();
				const auto at = point(path(leafName), {.5, .5});
				mouse(QEvent::MouseButtonPress, at);
				mouse(QEvent::MouseMove, at + QPointF(25, 30));
				if (interruption == 0) {
					QTest::keyClick(display, Qt::Key_Escape);
				} else if (interruption == 1) {
					get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(0);
				} else if (interruption == 2) {
					obs_sceneitem_set_locked(path(groupName).item, true);
				} else {
					obs_sceneitem_set_visible(path(groupName).item, false);
				}
				QTest::qWait(150);
				mouse(QEvent::MouseButtonRelease, at + QPointF(25, 30));
				check(equal(composition(), before, .4),
				      "Esc context change lock or hidden ancestor cancels full child gesture",
				      {{"interruption", interruption}, {"actual", composition()}, {"expected", before}});
				obs_sceneitem_set_locked(path(groupName).item, false);
				obs_sceneitem_set_visible(path(groupName).item, true);
				get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
			}
			treeSelect(leafName);
			const auto roundtripBefore = composition();
			const auto roundtripAt = point(path(leafName), {.5, .5});
			const auto roundtripUndo = get<QAction>(window, "actionMainUndo")->text();
			mouse(QEvent::MouseButtonPress, roundtripAt);
			mouse(QEvent::MouseMove, roundtripAt + QPointF(21, 26));
			// Deliberately no event-loop turn between these two context changes.
			const bool leftContext = QMetaObject::invokeMethod(window, "SetStudioPortraitEditing",
									   Qt::DirectConnection, Q_ARG(bool, false));
			const bool returnedContext = QMetaObject::invokeMethod(window, "SetStudioPortraitEditing",
									       Qt::DirectConnection, Q_ARG(bool, true));
			check(leftContext && returnedContext,
			      "rapid context roundtrip invokes both direct slots without an event-loop turn");
			mouse(QEvent::MouseMove, roundtripAt + QPointF(45, 53));
			mouse(QEvent::MouseButtonRelease, roundtripAt + QPointF(45, 53));
			check(equal(roundtripBefore, composition(), .4) &&
				      get<QAction>(window, "actionMainUndo")->text() == roundtripUndo,
			      "rapid Main to Portrait context roundtrip synchronously cancels child gesture without history");
			for (bool hidden : {false, true}) {
				treeSelect(leafName);
				const auto before = composition();
				const auto at = point(path(leafName), {.5, .5});
				const auto actionBefore = get<QAction>(window, "actionMainUndo")->text();
				if (hidden) {
					obs_sceneitem_set_visible(path(groupName).item, false);
				} else {
					obs_sceneitem_set_locked(path(groupName).item, true);
				}
				QTest::qWait(120);
				check(wait([] { return !get<QDoubleSpinBox>(window, "studioPositionX")->isEnabled(); }),
				      "numeric inspector disables child transform when an ancestor is locked or hidden");
				check(!get<QPushButton>(window, "studioSourceTransform")->isEnabled(),
				      "advanced transform button inherits the blocked ancestor section state");
				gesture(at, at + QPointF(25, 35));
				check(equal(before, composition(), .4) &&
					      get<QAction>(window, "actionMainUndo")->text() == actionBefore,
				      "already hidden or locked ancestor prevents child mutation and history",
				      {{"hidden", hidden}});
				obs_sceneitem_set_visible(path(groupName).item, true);
				obs_sceneitem_set_locked(path(groupName).item, false);
			}
			for (int variant = 0; variant < 4; ++variant) {
				const std::string group = "Portrait Groups QA partial " + std::to_string(variant);
				const std::string left = group + " left", right = group + " right";
				for (const auto &name : {group, left, right}) {
					OBSSourceAutoRelease collision = obs_get_source_by_name(name.c_str());
					check(!collision, "partial-removal namespace is unused");
				}
				auto *groupItem = obs_scene_add_group2(portrait(), group.c_str(), true);
				registerTemporary(obs_sceneitem_get_source(groupItem));
				for (const auto &name : {left, right}) {
					OBSDataAutoRelease settings = obs_data_create();
					obs_data_set_int(settings, "width", 100);
					obs_data_set_int(settings, "height", 100);
					obs_data_set_int(settings, "color", 0xffdd8800);
					OBSSourceAutoRelease source =
						obs_source_create("color_source_v3", name.c_str(), settings, nullptr);
					registerTemporary(source);
					setPose(obs_scene_add(obs_sceneitem_group_get_scene(groupItem), source),
						name == left ? QPointF(0, 0) : QPointF(200, 0));
				}
				QTest::qWait(180);
				auto groupInfo = info(groupItem);
				groupInfo.alignment = variant < 2 ? OBS_ALIGN_CENTER
								  : OBS_ALIGN_RIGHT | OBS_ALIGN_BOTTOM;
				groupInfo.pos = variant < 2 ? vec2{500, 1400} : vec2{850, 1650};
				obs_sceneitem_set_info2(groupItem, &groupInfo);
				QTest::qWait(180);
				QMetaObject::invokeMethod(get<QListView>(window, "sources"), "RefreshItems",
							  Qt::DirectConnection);
				const std::string removedName = variant % 2 == 0 ? left : right;
				const std::string survivorName = variant % 2 == 0 ? right : left;
				treeSelect(removedName.c_str());
				const QJsonValue survivorBefore = geometry(survivorName.c_str()).value("points");
				const auto removalAction = get<QAction>(window, "actionMainUndo")->text();
				OBSSceneItem extreme = path(removedName.c_str()).item;
				const auto at = point(path(removedName.c_str()), {.5, .5});
				mouse(QEvent::MouseButtonPress, at);
				mouse(QEvent::MouseMove, at + QPointF(25, 31));
				obs_sceneitem_remove(extreme);
				QTest::qWait(180);
				mouse(QEvent::MouseButtonRelease, at + QPointF(25, 31));
				Path missing;
				check(!obs_sceneitem_get_scene(extreme) &&
					      !find(portrait(), removedName.c_str(), missing),
				      "cancel after removing either extreme child never resurrects it",
				      {{"variant", variant}});
				check(equal(geometry(survivorName.c_str()).value("points"), survivorBefore, .4) &&
					      get<QAction>(window, "actionMainUndo")->text() == removalAction,
				      "partial cancellation preserves survivor world geometry for CENTER and RIGHT BOTTOM groups without history",
				      {{"variant", variant},
				       {"actual", geometry(survivorName.c_str()).value("points")},
				       {"expected", survivorBefore}});
				for (const auto &name : {group, left, right}) {
					OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
					if (source) {
						obs_source_remove(source);
					}
				}
				QTest::qWait(180);
			}
			treeSelect(leafName);
			OBSSceneItem removed = path(leafName).item;
			OBSScene removedOwner = obs_sceneitem_get_scene(removed);
			const auto removedInfo = info(removed);
			const auto removedCrop = crop(removed);
			const QJsonValue survivingChild = geometry(childName).value("points");
			const QJsonValue survivingSibling = geometry(siblingName).value("points");
			const auto removalAt = point(path(leafName), {.5, .5});
			mouse(QEvent::MouseButtonPress, removalAt);
			mouse(QEvent::MouseMove, removalAt + QPointF(27, 34));
			obs_sceneitem_remove(removed);
			QTest::qWait(180);
			mouse(QEvent::MouseButtonRelease, removalAt + QPointF(27, 34));
			Path shouldNotExist;
			check(!obs_sceneitem_get_scene(removed) && !find(portrait(), leafName, shouldNotExist),
			      "removing a live child participant cancels without resurrecting it");
			check(equal(geometry(childName).value("points"), survivingChild, .4) &&
				      equal(geometry(siblingName).value("points"), survivingSibling, .4),
			      "cancel after child removal retains surviving siblings in global canvas coordinates");
			OBSSourceAutoRelease restoredLeaf = obs_get_source_by_name(leafName);
			auto *replacement = obs_scene_add(removedOwner, restoredLeaf);
			obs_sceneitem_defer_update_begin(replacement);
			obs_sceneitem_set_info2(replacement, &removedInfo);
			obs_sceneitem_set_crop(replacement, &removedCrop);
			obs_sceneitem_defer_update_end(replacement);
			QTest::qWait(250);
			// ERR-029: one ordinary recursive prune must propagate a rebuilt
			// nested box to the external group's source dimensions. Do not
			// force ancestor flags or retry until the expected size appears.
			checkGroupExtents(
				groupName,
				"one normal prune propagates nested replacement extents into the external group dimensions");
			QMetaObject::invokeMethod(get<QListView>(window, "sources"), "RefreshItems",
						  Qt::DirectConnection);
			// History must re-check ownership at execution time. Persist each
			// observed phase before assertions so failures remain diagnosable.
			treeSelect(childName);
			const auto sharedHistoryBefore = composition();
			QJsonObject sharedHistoryAfter;
			OBSSource sharedGroup = obs_sceneitem_get_source(path(groupName).item);
			OBSSceneItem lateMainReference;
			QJsonArray lateSharingPhases;
			auto sharingPhase = [&](const char *phase) {
				QJsonObject observed = historyDiagnostic();
				observed["phase"] = phase;
				observed["reference_attached"] = lateMainReference &&
								 obs_sceneitem_get_scene(lateMainReference);
				observed["reference_item_id"] =
					lateMainReference ? double(obs_sceneitem_get_id(lateMainReference)) : -1;
				lateSharingPhases.append(observed);
				write("diagnostic-late-sharing.json", {{"before_gesture", sharedHistoryBefore},
								       {"after_gesture", sharedHistoryAfter},
								       {"phases", lateSharingPhases}});
				return observed;
			};
			sharingPhase("before_gesture");
			const auto sharedAt = point(path(childName), {.5, .5});
			gesture(sharedAt, sharedAt + QPointF(13, 18));
			sharedHistoryAfter = composition();
			const auto sharingAfterGesture = sharingPhase("after_gesture");
			check(!equal(sharedHistoryBefore, sharedHistoryAfter),
			      "late-sharing history precondition contains a real completed child gesture",
			      sharingAfterGesture);
			lateMainReference = obs_scene_add(obs_scene_from_source(main), sharedGroup);
			const auto sharingAfterAdd = sharingPhase("main_reference_added_immediate");
			check(lateMainReference &&
				      obs_sceneitem_get_scene(lateMainReference) == obs_scene_from_source(main) &&
				      obs_sceneitem_get_source(lateMainReference) == sharedGroup.Get() &&
				      obs_sceneitem_is_group(lateMainReference),
			      "late-sharing fixture references the identical group source from Main", sharingAfterAdd);
			QTest::qWait(180);
			const auto lateSharedState = composition();
			sharingPhase("main_reference_added_settled");
			get<QAction>(window, "actionMainUndo")->trigger();
			sharingPhase("shared_undo_immediate");
			QTest::qWait(180);
			auto sharingAfterUndo = sharingPhase("shared_undo_settled");
			sharingAfterUndo["expected"] = lateSharedState;
			check(equal(lateSharedState, composition(), .4) &&
				      get<QAction>(window, "actionMainRedo")->isEnabled(),
			      "Undo never mutates a branch that became shared with Main after the gesture",
			      sharingAfterUndo);
			get<QAction>(window, "actionMainRedo")->trigger();
			sharingPhase("shared_redo_immediate");
			QTest::qWait(180);
			auto sharingAfterRedo = sharingPhase("shared_redo_settled");
			sharingAfterRedo["expected"] = lateSharedState;
			check(equal(lateSharedState, composition(), .4),
			      "Redo also preserves a branch that became shared with Main", sharingAfterRedo);
			obs_sceneitem_remove(lateMainReference);
			const auto sharingAfterRemove = sharingPhase("main_reference_removed_immediate");
			check(!obs_sceneitem_get_scene(lateMainReference),
			      "late Main reference is removed after the isolation check", sharingAfterRemove);
			QTest::qWait(180);
			sharingPhase("main_reference_removed_settled");
			get<QAction>(window, "actionMainUndo")->trigger();
			sharingPhase("unlinked_undo_immediate");
			const bool unlinkedUndoRestored =
				wait([&] { return equal(sharedHistoryBefore, composition(), .4); });
			auto sharingAfterUnlinkedUndo = sharingPhase("unlinked_undo_wait_result");
			sharingAfterUnlinkedUndo["expected"] = sharedHistoryBefore;
			check(unlinkedUndoRestored,
			      "after unlinking Main the same one-step Undo reaches the original child gesture baseline",
			      sharingAfterUnlinkedUndo);
			get<QAction>(window, "actionMainRedo")->trigger();
			sharingPhase("unlinked_redo_immediate");
			const bool unlinkedRedoRestored =
				wait([&] { return equal(sharedHistoryAfter, composition(), .4); });
			auto sharingAfterUnlinkedRedo = sharingPhase("unlinked_redo_wait_result");
			sharingAfterUnlinkedRedo["expected"] = sharedHistoryAfter;
			check(unlinkedRedoRestored,
			      "after unlinking Main the same one-step Redo reapplies the exact original gesture",
			      sharingAfterUnlinkedRedo);

			// Moving a nested group changes its branch while its children's
			// owner UUID/item IDs remain unchanged. History must not follow them.
			treeSelect(leafName);
			const auto movedHistoryBefore = composition();
			QJsonArray reparentPhases;
			auto reparentPhase = [&](const char *phase, QJsonObject extra = {}) {
				QJsonObject observed = historyDiagnostic();
				observed["phase"] = phase;
				for (auto it = extra.begin(); it != extra.end(); ++it) {
					observed[it.key()] = it.value();
				}
				reparentPhases.append(observed);
				write("diagnostic-reparent.json",
				      {{"before_gesture", movedHistoryBefore}, {"phases", reparentPhases}});
				return observed;
			};
			reparentPhase("before_gesture");
			const auto movedAt = point(path(leafName), {.5, .5});
			gesture(movedAt, movedAt + QPointF(17, 23));
			check(!equal(movedHistoryBefore, composition()),
			      "reparent history precondition contains a real completed nested-child gesture",
			      reparentPhase("after_gesture"));
			const char *destinationName = "Portrait Groups QA reparent destination";
			OBSSourceAutoRelease destinationCollision = obs_get_source_by_name(destinationName);
			check(!destinationCollision, "reparent destination namespace is unused",
			      reparentPhase("before_destination_create", {{"destination_name", destinationName}}));
			OBSSceneItem destination = obs_scene_add_group2(portrait(), destinationName, true);
			registerTemporary(obs_sceneitem_get_source(destination));
			OBSSceneItem movedGroup = path(nestedName).item;
			OBSSceneItem oldParent = path(groupName).item;
			const std::string originalNestedOwner =
				obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(movedGroup)));
			const std::string originalLeafOwner =
				obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(path(leafName).item)));
			const auto movedSource = obs_sceneitem_get_source(movedGroup);
			obs_enter_graphics();
			obs_sceneitem_group_remove_item(oldParent, movedGroup);
			obs_sceneitem_group_add_item(destination, movedGroup);
			obs_scene_prune_sources(portrait());
			obs_leave_graphics();
			QTest::qWait(180);
			check(obs_sceneitem_get_scene(movedGroup) == obs_sceneitem_group_get_scene(destination) &&
				      originalNestedOwner != obs_source_get_uuid(obs_scene_get_source(
								     obs_sceneitem_get_scene(movedGroup))) &&
				      originalLeafOwner == obs_source_get_uuid(obs_scene_get_source(
								   obs_sceneitem_get_scene(path(leafName).item))) &&
				      obs_sceneitem_get_source(movedGroup) == movedSource,
			      "external reparent changes ancestor branch while preserving leaf owner UUID and shared source identity",
			      reparentPhase(
				      "after_external_reparent",
				      {{"original_nested_owner_uuid", QString::fromStdString(originalNestedOwner)},
				       {"original_leaf_owner_uuid", QString::fromStdString(originalLeafOwner)},
				       {"actual_nested_owner_uuid",
					obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(movedGroup)))},
				       {"actual_leaf_owner_uuid",
					obs_source_get_uuid(
						obs_scene_get_source(obs_sceneitem_get_scene(path(leafName).item)))},
				       {"destination_uuid",
					obs_source_get_uuid(obs_sceneitem_get_source(destination))}}));
			auto externalLeafInfo = info(path(leafName).item);
			const float externalLeafRotation = externalLeafInfo.rot + 11;
			externalLeafInfo.rot = externalLeafRotation;
			auto externalLeafCrop = crop(path(leafName).item);
			++externalLeafCrop.left;
			obs_enter_graphics();
			obs_sceneitem_set_info2(path(leafName).item, &externalLeafInfo);
			obs_sceneitem_set_crop(path(leafName).item, &externalLeafCrop);
			obs_scene_prune_sources(portrait());
			obs_leave_graphics();
			QTest::qWait(180);
			check(std::abs(info(path(leafName).item).rot - externalLeafRotation) < .05 &&
				      crop(path(leafName).item).left == externalLeafCrop.left,
			      "reparented leaf has a distinct external rotation and crop before old history executes",
			      reparentPhase("after_external_leaf_edit",
					    {{"expected_rotation", externalLeafRotation},
					     {"expected_crop_left", externalLeafCrop.left}}));
			auto movedState = [&] {
				return QJsonObject{{"nested", geometry(nestedName)},
						   {"leaf", geometry(leafName)},
						   {"destination", geometry(destinationName)}};
			};
			checkGroupExtents(
				nestedName,
				"nested extents are coherent after normal external leaf setters before reparent history baseline");
			const auto movedBaseline = movedState();
			reparentPhase("before_undo", {{"expected_moved_state", movedBaseline}});
			get<QAction>(window, "actionMainUndo")->trigger();
			reparentPhase("undo_immediate", {{"moved_state", movedState()}});
			QTest::qWait(180);
			check(equal(movedBaseline, movedState(), .4) &&
				      get<QAction>(window, "actionMainRedo")->isEnabled(),
			      "Undo does not follow a reparented nested group or its children into another branch",
			      reparentPhase("undo_settled", {{"expected_moved_state", movedBaseline},
							     {"actual_moved_state", movedState()}}));
			get<QAction>(window, "actionMainRedo")->trigger();
			reparentPhase("redo_immediate", {{"moved_state", movedState()}});
			QTest::qWait(180);
			check(equal(movedBaseline, movedState(), .4),
			      "Redo also leaves the externally reparented branch unchanged",
			      reparentPhase("redo_settled", {{"expected_moved_state", movedBaseline},
							     {"actual_moved_state", movedState()}}));
			obs_enter_graphics();
			obs_sceneitem_group_remove_item(destination, movedGroup);
			obs_sceneitem_group_add_item(oldParent, movedGroup);
			obs_scene_prune_sources(portrait());
			obs_leave_graphics();
			obs_sceneitem_remove(destination);
			OBSSourceAutoRelease destinationSource = obs_get_source_by_name(destinationName);
			if (destinationSource) {
				obs_source_remove(destinationSource);
			}
			QTest::qWait(180);
			check(obs_sceneitem_get_scene(movedGroup) == obs_sceneitem_group_get_scene(oldParent) &&
				      !obs_sceneitem_get_scene(destination),
			      "reparent fixture restores the original branch and removes its temporary destination",
			      reparentPhase("cleanup_complete"));
			check(equal(mainBefore, mainState()),
			      "all child gestures leave shared Main transforms and crop intact");
			for (const char *name : {childName, siblingName, leafName}) {
				check(obs_sceneitem_get_source(path(name).item) ==
					      obs_sceneitem_get_source(
						      obs_scene_find_source(obs_scene_from_source(main), name)),
				      "group child retains its original shared source pointer", {{"name", name}});
			}
			checkGroupExtents(nestedName,
					  "nested extents remain coherent before final persistence baseline");
			checkGroupExtents(groupName, "outer extents remain coherent before final persistence baseline");
			saved["final"] = composition();
			saved["main_before"] = mainBefore;
			saved["portrait_uuid"] = obs_source_get_uuid(obs_scene_get_source(portrait()));
			for (const char *name : {groupName, nestedName, childName, siblingName, leafName}) {
				const auto item = path(name).item;
				saved[QString(name) + "_identity"] = QJsonObject{
					{"item", double(obs_sceneitem_get_id(item))},
					{"owner",
					 obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(item)))},
					{"source", obs_source_get_uuid(obs_sceneitem_get_source(item))}};
			}
			write("saved.json", saved);
			obs_frontend_save();
			check(crop(path(childName).item).left > 0 && crop(path(leafName).item).left > 0,
			      "restart fixture saves actual direct and nested child crop");
			display->grab().save(root + "/preview.png");
		}
	} catch (const std::exception &exception) {
		error = exception.what();
		if (!recovering) {
			try {
				cleanup();
			} catch (...) {
			}
		}
	}
	write(recovering  ? "recovery.json"
	      : restoring ? "restore.json"
			  : "runtime.json",
	      {{"passed", error.isEmpty()},
	       {"error", error},
	       {"checks", checks},
	       {"device_pixel_ratio", display ? display->devicePixelRatioF() : 0},
	       {"scope",
		recovering
			? "Fixture-only cleanup from the saved private baseline; no new fixture, input gesture or media output."
			: "Injected Qt gestures on disposable libobs direct/nested groups; native history and process restart. No Windows-native, physical-input, capture-device, encoder-concurrency or prolonged-runtime certification."}});
}
void event(obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(1800, qApp, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_PORTRAIT_GROUPS_ROOT");
	restoring = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_GROUPS_RESTORE");
	recovering = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_GROUPS_RECOVER");
	if (root.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
