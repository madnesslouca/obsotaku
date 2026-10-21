/* Opt-in native Qt/libobs single-item rotation. Only disposable sources are
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
#include <stdexcept>
#include <vector>

OBS_DECLARE_MODULE()
namespace {
constexpr const char *sceneName = "Portrait Rotation QA";
constexpr const char *groupName = "Portrait Rotation QA group";
constexpr const char *nestedName = "Portrait Rotation QA nested";
constexpr const char *childName = "Portrait Rotation QA child";
constexpr const char *siblingName = "Portrait Rotation QA sibling";
constexpr const char *leafName = "Portrait Rotation QA leaf";
constexpr const char *rootName = "Portrait Rotation QA root";
QString root;
QMainWindow *window = nullptr;
QWidget *display = nullptr;
QDockWidget *dock = nullptr;
QJsonObject saved;
QJsonArray checks;
bool restoring = false;
std::string groupLookupName = groupName;
std::vector<std::string> temporaryNames;
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
	blog(LOG_INFO, "[portrait-rotation-check] %s %s %s", pass ? "PASS" : "FAIL", label,
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
	if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		OBSSourceAutoRelease active = obs_transition_get_active_source(source);
		return active ? obs_scene_from_source(active) : nullptr;
	}
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
	for (const char *name : {groupName, childName, siblingName, nestedName, leafName, rootName}) {
		result[name] = geometry(name);
	}
	return result;
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
void mouse(QEvent::Type type, QPointF canvas, Qt::KeyboardModifiers modifiers = {})
{
	const QPointF p = pixel(canvas);
	QMouseEvent event(type, p, p, display->mapToGlobal(p),
			  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
			  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
	QApplication::sendEvent(display, &event);
	QTest::qWait(45);
}
void gesture(QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = {})
{
	ready();
	mouse(QEvent::MouseButtonPress, from, modifiers);
	for (int n = 1; n <= 6; ++n) {
		mouse(QEvent::MouseMove, from + (to - from) * n / 6., modifiers);
	}
	mouse(QEvent::MouseButtonRelease, to, modifiers);
	QTest::qWait(180);
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
	for (const auto &name : temporaryNames) {
		OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
		if (source) {
			obs_source_remove(source);
		}
	}
	for (const char *name : {sceneName, groupName, nestedName, childName, siblingName, leafName, rootName}) {
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
	for (const char *name : {sceneName, groupName, nestedName, childName, siblingName, leafName, rootName}) {
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
	for (const char *name : {childName, siblingName, leafName, rootName}) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "width", name == childName ? 360 : 120);
		obs_data_set_int(settings, "height", name == childName ? 300 : 150);
		obs_data_set_int(settings, "color", name == childName ? 0xff00aaff : 0xff66dd22);
		OBSSourceAutoRelease source = obs_source_create("color_source_v3", name, settings, nullptr);
		check(bool(source), "disposable child shares a real color source with Main");
		setPose(obs_scene_add(main, source), {30, 50});
	}
	go(obs_scene_get_source(main));
	OBSSourceAutoRelease rootSource = obs_get_source_by_name(rootName);
	setPose(obs_scene_add(portrait(), rootSource), {730, 1350});
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
QPointF ownerPoint(const Path &p, QPointF canvas)
{
	for (const auto &ancestor : p.ancestors) {
		matrix4 content, inverse;
		obs_sceneitem_get_content_transform(ancestor, &content);
		if (!matrix4_inv(&inverse, &content)) {
			throw std::runtime_error("noninvertible fixture owner");
		}
		canvas = map(inverse, canvas);
	}
	return canvas;
}
QPointF canvasPoint(const Path &p, QPointF owner)
{
	for (auto it = p.ancestors.rbegin(); it != p.ancestors.rend(); ++it) {
		matrix4 content;
		obs_sceneitem_get_content_transform(*it, &content);
		owner = map(content, owner);
	}
	return owner;
}
double angleDifference(double a, double b)
{
	return std::remainder(a - b, 360.);
}
struct Grip {
	QPointF point, center;
};
Grip rotationGrip()
{
	check(wait([] {
		      const QVariant handle = display->property("studioPortraitRotationHandle");
		      const QVariant center = display->property("studioPortraitRotationCenter");
		      const QPointF h = handle.toPointF(), c = center.toPointF();
		      return handle.isValid() && center.isValid() && handle.canConvert<QPointF>() &&
			     center.canConvert<QPointF>() && std::isfinite(h.x()) && std::isfinite(h.y()) &&
			     std::isfinite(c.x()) && std::isfinite(c.y()) && QLineF(h, c).length() > 1;
	      }),
	      "one editable selection exposes a separate rotation handle and center in canvas coordinates");
	return {display->property("studioPortraitRotationHandle").toPointF(),
		display->property("studioPortraitRotationCenter").toPointF()};
}
QPointF rotationEnd(const Path &p, const Grip &grip, double delta)
{
	const auto center = ownerPoint(p, grip.center), start = ownerPoint(p, grip.point) - center;
	const double radians = delta * std::acos(-1.) / 180.;
	return canvasPoint(p, center + QPointF(start.x() * std::cos(radians) - start.y() * std::sin(radians),
					       start.x() * std::sin(radians) + start.y() * std::cos(radians)));
}
void rotate(const char *target, double delta, bool snap = false, bool rename = false)
{
	treeSelect(target);
	const auto p = path(target);
	const auto grip = rotationGrip();
	const auto before = composition();
	const auto initial = info(p.item);
	const auto initialCrop = crop(p.item);
	const auto end = rotationEnd(p, grip, delta);
	const auto a = ownerPoint(p, grip.point) - ownerPoint(p, grip.center);
	const auto b = ownerPoint(p, end) - ownerPoint(p, grip.center);
	const double independentDelta = angleDifference(std::atan2(b.y(), b.x()) * 180 / std::acos(-1.),
							std::atan2(a.y(), a.x()) * 180 / std::acos(-1.));
	const double expected = snap ? std::round((initial.rot + independentDelta) / 15.) * 15
				     : initial.rot + independentDelta;
	check(QLineF(point(p, {.5, .5}), grip.center).length() < 1.2,
	      "rotation pivot equals independent item box center");
	const auto modifiers = snap ? Qt::ShiftModifier : Qt::NoModifier;
	mouse(QEvent::MouseButtonPress, grip.point, modifiers);
	for (int n = 1; n <= 6; ++n) {
		mouse(QEvent::MouseMove, grip.point + (end - grip.point) * n / 6., modifiers);
	}
	check(QLineF(point(path(target), {.5, .5}), grip.center).length() < 1.2,
	      "rotation keeps the canvas pivot fixed while ancestor resizing is frozen",
	      {{"target", target},
	       {"actual_x", point(path(target), {.5, .5}).x()},
	       {"actual_y", point(path(target), {.5, .5}).y()},
	       {"pivot_x", grip.center.x()},
	       {"pivot_y", grip.center.y()}});
	mouse(QEvent::MouseButtonRelease, end, modifiers);
	QTest::qWait(180);
	const auto current = info(path(target).item);
	check(std::abs(angleDifference(current.rot, expected)) < .15,
	      "rotation follows independent owner-local atan2 and absolute 15 degree snapping",
	      {{"target", target},
	       {"initial", initial.rot},
	       {"delta", independentDelta},
	       {"expected", expected},
	       {"actual", current.rot},
	       {"snap", snap}});
	const auto currentCrop = crop(path(target).item);
	check(std::abs(current.scale.x - initial.scale.x) < .005 &&
		      std::abs(current.scale.y - initial.scale.y) < .005 &&
		      current.bounds_type == initial.bounds_type &&
		      std::abs(current.bounds.x - initial.bounds.x) < .02 &&
		      std::abs(current.bounds.y - initial.bounds.y) < .02 && currentCrop.left == initialCrop.left &&
		      currentCrop.top == initialCrop.top && currentCrop.right == initialCrop.right &&
		      currentCrop.bottom == initialCrop.bottom,
	      "rotation preserves scale bounds and source-pixel crop");
	const auto after = composition();
	if (rename) {
		OBSSource group = obs_sceneitem_get_source(path(groupName).item);
		groupLookupName = "Portrait Rotation QA renamed";
		obs_source_set_name(group, groupLookupName.c_str());
		history(before, after);
		check(QString::fromUtf8(obs_source_get_name(group)) == QString::fromStdString(groupLookupName),
		      "rotation history preserves a later ancestor rename by UUID");
		obs_source_set_name(group, groupName);
		groupLookupName = groupName;
	} else {
		history(before, after);
	}
}
void run()
{
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active(), "rotation fixture starts with every encoder idle");
		if (restoring) {
			QFile file(root + "/saved.json");
			check(file.open(QIODevice::ReadOnly), "private rotation restart baseline exists");
			saved = QJsonDocument::fromJson(file.readAll()).object();
			mode(true);
			OBSSourceAutoRelease main = obs_get_source_by_name(sceneName);
			check(bool(main), "rotation restart restores fixture Main");
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
		ready();
		OBSSourceAutoRelease main = obs_get_source_by_name(sceneName);
		auto mainState = [&] {
			QJsonObject result;
			for (const char *name : {childName, siblingName, leafName, rootName}) {
				result[name] = state(obs_scene_find_source(obs_scene_from_source(main), name));
			}
			return result;
		};
		const auto mainBefore = mainState();
		if (restoring) {
			check(saved["portrait_uuid"].toString() ==
				      obs_source_get_uuid(obs_scene_get_source(portrait())),
			      "rotation restart retains portrait UUID");
			check(equal(composition(), saved["final"], .35),
			      "rotation restart retains complete graph transforms crop and bounds",
			      {{"actual", composition()}, {"expected", saved["final"]}});
			check(equal(mainBefore, saved["main_before"]),
			      "rotation restart preserves all original Main transforms");
			for (const char *name : {groupName, nestedName, childName, siblingName, leafName, rootName}) {
				const auto item = path(name).item;
				const auto identity = saved[QString(name) + "_identity"].toObject();
				check(identity["item"].toDouble() == obs_sceneitem_get_id(item) &&
					      identity["owner"].toString() == obs_source_get_uuid(obs_scene_get_source(
										      obs_sceneitem_get_scene(item))) &&
					      identity["source"].toString() ==
						      obs_source_get_uuid(obs_sceneitem_get_source(item)),
				      "rotation restart retains owner UUID item ID and source identity",
				      {{"name", name}});
			}
			treeSelect(rootName);
			rotationGrip();
			cleanup();
		} else {
			treeSelect(rootName);
			const auto unchangedGrip = rotationGrip();
			const auto unchangedBefore = composition();
			const auto unchangedUndo = get<QAction>(window, "actionMainUndo")->text();
			gesture(unchangedGrip.point, unchangedGrip.point);
			check(equal(unchangedBefore, composition()) &&
				      get<QAction>(window, "actionMainUndo")->text() == unchangedUndo,
			      "rotation handle click without movement creates no history");
			rotate(rootName, 17);
			rotate(rootName, -28);
			rotate(groupName, 19);
			setPose(path(groupName).item, {190, 460}, 23);
			obs_sceneitem_crop manual{};
			manual.left = 20;
			manual.top = 10;
			obs_sceneitem_set_crop(path(groupName).item, &manual);
			QTest::qWait(180);
			rotate(childName, 31, false, true);
			setPose(path(groupName).item, {900, 460}, 17, true);
			const vec2 parentScale{-.8f, 1.3f};
			obs_sceneitem_set_scale(path(groupName).item, &parentScale);
			obs_sceneitem_set_crop(path(groupName).item, &manual);
			auto bounded = info(path(childName).item);
			bounded.rot = 14;
			bounded.bounds_type = OBS_BOUNDS_SCALE_INNER;
			bounded.bounds = {300, 240};
			obs_sceneitem_set_info2(path(childName).item, &bounded);
			obs_sceneitem_crop childCrop{};
			childCrop.left = 25;
			childCrop.top = 15;
			obs_sceneitem_set_crop(path(childName).item, &childCrop);
			QTest::qWait(180);
			rotate(childName, -27);
			auto nested = info(path(nestedName).item);
			nested.rot = -12;
			nested.scale = {.9f, 1.1f};
			obs_sceneitem_set_info2(path(nestedName).item, &nested);
			auto leaf = info(path(leafName).item);
			leaf.rot = 8;
			obs_sceneitem_set_info2(path(leafName).item, &leaf);
			obs_sceneitem_crop leafCrop{};
			leafCrop.left = 3;
			leafCrop.top = 5;
			obs_sceneitem_set_crop(path(leafName).item, &leafCrop);
			QTest::qWait(180);
			rotate(leafName, 27, true, true);
			auto rootBounds = info(path(rootName).item);
			rootBounds.rot = 14;
			rootBounds.bounds_type = OBS_BOUNDS_SCALE_OUTER;
			rootBounds.bounds = {90, 130};
			rootBounds.crop_to_bounds = true;
			obs_sceneitem_set_info2(path(rootName).item, &rootBounds);
			QTest::qWait(180);
			rotate(rootName, 20, true);
			treeSelect(rootName);
			auto *tree = get<QListView>(window, "sources");
			for (int n = 0; n < tree->model()->rowCount(); ++n) {
				const auto index = tree->model()->index(n, 0);
				if (index.data(Qt::AccessibleTextRole).toString() == groupName) {
					tree->selectionModel()->select(index, QItemSelectionModel::Select |
										      QItemSelectionModel::Rows);
				}
			}
			check(wait([] {
				      return !display->property("studioPortraitRotationHandle").isValid() &&
					     !display->property("studioPortraitRotationCenter").isValid();
			      }),
			      "multiple selections expose no collective rotation handle");
			for (int interruption = 0; interruption < 4; ++interruption) {
				treeSelect(childName);
				const auto grip = rotationGrip();
				const auto before = composition();
				const auto action = get<QAction>(window, "actionMainUndo")->text();
				const auto end = rotationEnd(path(childName), grip, 22);
				mouse(QEvent::MouseButtonPress, grip.point);
				mouse(QEvent::MouseMove, end);
				check(!equal(before, composition()), "rotation is active before interruption is tested",
				      {{"interruption", interruption}});
				if (interruption == 0) {
					QTest::keyClick(display, Qt::Key_Escape);
				} else if (interruption == 1) {
					QMetaObject::invokeMethod(window, "SetStudioPortraitEditing",
								  Qt::DirectConnection, Q_ARG(bool, false));
					QMetaObject::invokeMethod(window, "SetStudioPortraitEditing",
								  Qt::DirectConnection, Q_ARG(bool, true));
				} else if (interruption == 2) {
					obs_sceneitem_set_locked(path(groupName).item, true);
				} else {
					obs_sceneitem_set_visible(path(groupName).item, false);
				}
				QTest::qWait(140);
				mouse(QEvent::MouseButtonRelease, end);
				check(equal(before, composition(), .4) &&
					      get<QAction>(window, "actionMainUndo")->text() == action,
				      "rotation cancellation restores full geometry without history",
				      {{"interruption", interruption}, {"actual", composition()}, {"expected", before}});
				obs_sceneitem_set_locked(path(groupName).item, false);
				obs_sceneitem_set_visible(path(groupName).item, true);
				get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
			}
			treeSelect(rootName);
			const auto grip = rotationGrip();
			const auto action = get<QAction>(window, "actionMainUndo")->text();
			OBSSceneItem removed = path(rootName).item;
			const auto oldInfo = info(removed);
			const auto oldCrop = crop(removed);
			const auto groupBefore = geometry(groupName);
			const auto end = rotationEnd(path(rootName), grip, 24);
			mouse(QEvent::MouseButtonPress, grip.point);
			mouse(QEvent::MouseMove, end);
			obs_sceneitem_remove(removed);
			QTest::qWait(140);
			mouse(QEvent::MouseButtonRelease, end);
			Path missing;
			check(!obs_sceneitem_get_scene(removed) && !find(portrait(), rootName, missing) &&
				      equal(groupBefore, geometry(groupName), .4) &&
				      get<QAction>(window, "actionMainUndo")->text() == action,
			      "removal cancels rotation without resurrecting participant or changing other groups");
			OBSSourceAutoRelease rootSource = obs_get_source_by_name(rootName);
			auto *replacement = obs_scene_add(portrait(), rootSource);
			obs_sceneitem_set_info2(replacement, &oldInfo);
			obs_sceneitem_set_crop(replacement, &oldCrop);
			QTest::qWait(180);
			rotate(rootName, 15);
			check(equal(mainBefore, mainState()),
			      "root group and child rotations preserve shared Main transforms and crop");
			for (const char *name : {childName, siblingName, leafName, rootName}) {
				check(obs_sceneitem_get_source(path(name).item) ==
					      obs_sceneitem_get_source(
						      obs_scene_find_source(obs_scene_from_source(main), name)),
				      "rotation preserves original shared source identity", {{"name", name}});
			}
			saved["final"] = composition();
			saved["main_before"] = mainBefore;
			saved["portrait_uuid"] = obs_source_get_uuid(obs_scene_get_source(portrait()));
			for (const char *name : {groupName, nestedName, childName, siblingName, leafName, rootName}) {
				const auto item = path(name).item;
				saved[QString(name) + "_identity"] = QJsonObject{
					{"item", double(obs_sceneitem_get_id(item))},
					{"owner",
					 obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(item)))},
					{"source", obs_source_get_uuid(obs_sceneitem_get_source(item))}};
			}
			write("saved.json", saved);
			obs_frontend_save();
			display->grab().save(root + "/preview.png");
		}
	} catch (const std::exception &exception) {
		error = exception.what();
		try {
			cleanup();
		} catch (...) {
		}
	}
	write(restoring ? "restore.json" : "runtime.json",
	      {{"passed", error.isEmpty()},
	       {"error", error},
	       {"checks", checks},
	       {"device_pixel_ratio", display ? display->devicePixelRatioF() : 0},
	       {"scope",
		"Injected Qt single-item rotation handle on synthetic root/group/direct/nested sources, owner-local geometry, native history and full process restart. No collective rotation, Windows-native, physical-pointer, capture-device, concurrent-output or long-duration certification."}});
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
	root = qEnvironmentVariable("OBS_STUDIO_PORTRAIT_ROTATION_ROOT");
	restoring = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_ROTATION_RESTORE");
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
