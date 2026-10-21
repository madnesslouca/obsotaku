/* Opt-in real Add-menu regression for independent Portrait group imports.
 * Qt input is injected; no device, stream or public account is involved. */
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
#include <QMainWindow>
#include <QMenu>
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
constexpr const char *sceneName = "Portrait Imports QA";
constexpr const char *groupName = "Portrait Imports QA group";
constexpr const char *nestedName = "Portrait Imports QA nested";
constexpr const char *childName = "Portrait Imports QA child";
constexpr const char *siblingName = "Portrait Imports QA sibling";
constexpr const char *leafName = "Portrait Imports QA leaf";
constexpr const char *effectName = "Portrait Imports QA effect";
QString root;
QMainWindow *window;
QWidget *display;
QDockWidget *dock;
QJsonObject saved;
QJsonArray checks;
bool restoring;
int historyIndex = 0;
int optionsMaximumHeight = 320;
template<class T> T *get(QObject *owner, const char *name)
{
	auto *value = owner->findChild<T *>(name);
	if (!value) {
		throw std::runtime_error(name);
	}
	return value;
}
void check(bool pass, const char *name, QJsonObject observed = {})
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}, {"observed", observed}});
	blog(LOG_INFO, "[portrait-imports-check] %s %s %s", pass ? "PASS" : "FAIL", name,
	     QJsonDocument(observed).toJson(QJsonDocument::Compact).constData());
	if (!pass) {
		throw std::runtime_error(name);
	}
}
bool wait(const std::function<bool()> &condition, int timeout = 3500)
{
	for (int ms = 0; ms < timeout; ms += 40) {
		if (condition()) {
			return true;
		}
		QTest::qWait(40);
	}
	return condition();
}
void write(const char *name, QJsonObject data)
{
	QFile file(root + "/" + name);
	if (!file.open(QIODevice::WriteOnly)) {
		throw std::runtime_error("private report unavailable");
	}
	file.write(QJsonDocument(data).toJson());
}
bool equal(QJsonValue a, QJsonValue b, double tolerance = .06)
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
		for (int i = 0; i < x.size(); ++i) {
			if (!equal(x[i], y[i], tolerance)) {
				return false;
			}
		}
		return true;
	}
	return a.isDouble() ? std::abs(a.toDouble() - b.toDouble()) <= tolerance : a == b;
}
QJsonObject json(obs_data_t *data)
{
	return data ? QJsonDocument::fromJson(obs_data_get_json(data)).object() : QJsonObject{};
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
OBSSource source(const char *name)
{
	OBSSourceAutoRelease value = obs_get_source_by_name(name);
	return value.Get();
}
OBSScene portrait()
{
	OBSCanvasAutoRelease canvas = obs_get_canvas_by_name("Vertical 9:16");
	OBSSourceAutoRelease s = canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
	if (s && obs_source_get_type(s) == OBS_SOURCE_TYPE_TRANSITION) {
		OBSSourceAutoRelease active = obs_transition_get_active_source(s);
		return obs_scene_from_source(active);
	}
	return obs_scene_from_source(s);
}
OBSSceneItem imported()
{
	const QString expected = saved["clone_uuid"].toString();
	for (const auto &item : items(portrait())) {
		if (QString::fromUtf8(obs_source_get_uuid(obs_sceneitem_get_source(item))) == expected) {
			return item;
		}
	}
	return nullptr;
}
OBSScene importedScene()
{
	OBSSceneItem item = imported();
	return item ? obs_sceneitem_group_get_scene(item) : nullptr;
}
OBSSceneItem child(const char *name)
{
	OBSScene owner = importedScene();
	return owner ? obs_scene_find_source(owner, name) : nullptr;
}
QJsonObject itemState(obs_sceneitem_t *item)
{
	obs_transform_info i{};
	obs_sceneitem_crop c{};
	obs_sceneitem_get_info2(item, &i);
	obs_sceneitem_get_crop(item, &c);
	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(item);
	return {{"x", i.pos.x},
		{"y", i.pos.y},
		{"sx", i.scale.x},
		{"sy", i.scale.y},
		{"rot", i.rot},
		{"align", int(i.alignment)},
		{"bounds_type", int(i.bounds_type)},
		{"bounds_align", int(i.bounds_alignment)},
		{"bounds_crop", i.crop_to_bounds},
		{"bw", i.bounds.x},
		{"bh", i.bounds.y},
		{"left", c.left},
		{"right", c.right},
		{"top", c.top},
		{"bottom", c.bottom},
		{"visible", obs_sceneitem_visible(item)},
		{"locked", obs_sceneitem_locked(item)},
		{"filter", int(obs_sceneitem_get_scale_filter(item))},
		{"blend", int(obs_sceneitem_get_blending_mode(item))},
		{"blend_method", int(obs_sceneitem_get_blending_method(item))},
		{"private", json(priv)}};
}
QJsonObject treeState(obs_source_t *group)
{
	OBSDataAutoRelease definition = obs_save_source(group);
	QJsonObject result{{"source", json(definition)}};
	QJsonArray descendants;
	for (const auto &item : items(obs_group_from_source(group))) {
		if (obs_sceneitem_is_group(item)) {
			descendants.append(treeState(obs_sceneitem_get_source(item)));
		}
	}
	result["groups"] = descendants;
	return result;
}
QJsonObject mainState()
{
	OBSSource group = source(groupName), nested = source(nestedName), main = source(sceneName);
	if (!group || !nested || !main) {
		return {};
	}
	OBSDataAutoRelease definition = obs_save_source(main);
	return {{"main", json(definition)}, {"group", treeState(group)}};
}
QJsonObject cloneState()
{
	OBSSceneItem item = imported();
	return item ? QJsonObject{{"outer", itemState(item)}, {"tree", treeState(obs_sceneitem_get_source(item))}}
		    : QJsonObject{};
}
void mode(bool enabled)
{
	bool ok = false;
	check(QMetaObject::invokeMethod(window, "SetStudioCanvasMode", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
					Q_ARG(bool, enabled)) &&
		      ok,
	      "idle canvas mode action succeeds");
}
void go(obs_source_t *main)
{
	obs_frontend_set_current_scene(main);
	check(wait([&] {
		      OBSScene s = portrait();
		      OBSDataAutoRelease priv = s ? obs_source_get_private_settings(obs_scene_get_source(s)) : nullptr;
		      return s && QString::fromUtf8(obs_data_get_string(priv, "otaku_main_scene_uuid")) ==
					  obs_source_get_uuid(main);
	      }),
	      "Portrait follows its requested Main UUID");
	QTest::qWait(200);
}
QPointF map(const matrix4 &matrix, QPointF p)
{
	vec3 in{}, out{};
	vec3_set(&in, p.x(), p.y(), 0);
	vec3_transform(&out, &in, &matrix);
	return {out.x, out.y};
}
QPointF point(obs_sceneitem_t *item, obs_sceneitem_t *group, QPointF unit)
{
	matrix4 box, parent;
	obs_sceneitem_get_box_transform(item, &box);
	QPointF result = map(box, unit);
	if (group) {
		obs_sceneitem_get_content_transform(group, &parent);
		result = map(parent, result);
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
	QSize lastDisplay, lastWindow;
	int stable = 0;
	const bool settled = wait(
		[&] {
			const QSize actualDisplay = display->size(), actualWindow = top->size();
			const bool exposed = top->windowHandle() && top->windowHandle()->isExposed();
			if (actualDisplay == lastDisplay && actualWindow == lastWindow && display->isVisible() &&
			    exposed && display->hasFocus() && actualDisplay.width() >= 180 &&
			    actualDisplay.height() >= 320) {
				++stable;
			} else {
				stable = 0;
			}
			lastDisplay = actualDisplay;
			lastWindow = actualWindow;
			return stable >= 18; // 720 ms stable after focus and compositor allocation.
		},
		6500);
	check(settled, "focused floating preview has a stable exposed frame before injected input",
	      {{"preview_width", display->width()},
	       {"preview_height", display->height()},
	       {"window_width", top->width()},
	       {"window_height", top->height()},
	       {"focused", display->hasFocus()}});
}
void mouse(QEvent::Type type, QPointF canvas, Qt::KeyboardModifiers modifiers = {})
{
	if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
		QApplication::setActiveWindow(display->window());
		display->setFocus(Qt::MouseFocusReason);
	}
	const QPointF p = pixel(canvas);
	QMouseEvent event(type, p, p, display->mapToGlobal(p),
			  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
			  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
	QApplication::sendEvent(display, &event);
	QTest::qWait(40);
}
void gesture(QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = {})
{
	ready();
	mouse(QEvent::MouseButtonPress, from, modifiers);
	for (int n = 1; n <= 5; ++n) {
		mouse(QEvent::MouseMove, from + (to - from) * n / 5., modifiers);
	}
	mouse(QEvent::MouseButtonRelease, to, modifiers);
	QTest::qWait(180);
}
void select(obs_sceneitem_t *expected)
{
	get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
	auto *tree = get<QListView>(window, "sources");
	QMetaObject::invokeMethod(tree, "RefreshItems", Qt::DirectConnection);
	const QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(expected)));
	check(wait([&] {
		      for (int row = 0; row < tree->model()->rowCount(); ++row) {
			      const auto index = tree->model()->index(row, 0);
			      if (index.data(Qt::AccessibleTextRole).toString() != name) {
				      continue;
			      }
			      tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
										     QItemSelectionModel::Rows);
			      if (obs_sceneitem_selected(expected)) {
				      return true;
			      }
		      }
		      return false;
	      }),
	      "native tree resolves the expected scene item, including duplicate leaf names");
	QTest::qWait(100);
	ready();
}
void history(const QJsonObject &before, const QJsonObject &after)
{
	check(!equal(before, after), "imported child gesture changes the backend");
	auto *undo = get<QAction>(window, "actionMainUndo");
	auto *redo = get<QAction>(window, "actionMainRedo");
	QJsonObject diagnostic{{"index", ++historyIndex},
			       {"before", before},
			       {"after", after},
			       {"undo_action_before", undo->text()},
			       {"redo_action_before", redo->text()},
			       {"undo_enabled_before", undo->isEnabled()},
			       {"redo_enabled_before", redo->isEnabled()},
			       {"scope_before", display->property("studioPortraitGroupScope").toString()},
			       {"gesture_status_before", display->property("studioPortraitGestureStatus").toString()}};
	auto differences = [](QJsonValue expected, QJsonValue actual) {
		QJsonArray result;
		std::function<void(QJsonValue, QJsonValue, const QString &)> compare = [&](QJsonValue a, QJsonValue b,
											   const QString &path) {
			if (equal(a, b, .3) || result.size() >= 128) {
				return;
			}
			if (a.isObject() && b.isObject()) {
				const auto x = a.toObject(), y = b.toObject();
				auto keys = x.keys();
				for (const auto &key : y.keys()) {
					if (!keys.contains(key)) {
						keys.append(key);
					}
				}
				for (const auto &key : keys) {
					compare(x[key], y[key], path + "/" + key);
				}
			} else if (a.isArray() && b.isArray() && a.toArray().size() == b.toArray().size()) {
				const auto x = a.toArray(), y = b.toArray();
				for (int i = 0; i < x.size(); ++i) {
					compare(x[i], y[i], path + "/" + QString::number(i));
				}
			} else {
				result.append(QJsonObject{{"path", path},
							  {"expected_absent", a.isUndefined()},
							  {"actual_absent", b.isUndefined()},
							  {"expected", a.isUndefined() ? QJsonValue{} : a},
							  {"actual", b.isUndefined() ? QJsonValue{} : b}});
			}
		};
		compare(expected, actual, QString{});
		return result;
	};
	undo->trigger();
	const bool undoRestored = wait([&] { return equal(cloneState(), before, .3); });
	const auto undoState = cloneState();
	diagnostic["undo_passed"] = undoRestored;
	diagnostic["undo_result"] = undoState;
	diagnostic["undo_differences"] = differences(before, undoState);
	diagnostic["undo_action_after"] = undo->text();
	diagnostic["redo_action_after_undo"] = redo->text();
	diagnostic["scope_after_undo"] = display->property("studioPortraitGroupScope").toString();
	diagnostic["main_preserved_after_undo"] = equal(mainState(), saved["main_before"], .06);
	auto report = [&] {
		write("history-diagnostic.json", diagnostic);
		write(QString("history-diagnostic-%1.json").arg(historyIndex).toUtf8().constData(), diagnostic);
	};
	report();
	check(undoRestored, "one Undo restores imported group geometry",
	      {{"diagnostic", "history-diagnostic.json"}, {"differences", diagnostic["undo_differences"]}});
	redo->trigger();
	const bool redoRestored = wait([&] { return equal(cloneState(), after, .3); });
	const auto redoState = cloneState();
	diagnostic["redo_passed"] = redoRestored;
	diagnostic["redo_result"] = redoState;
	diagnostic["redo_differences"] = differences(after, redoState);
	diagnostic["undo_action_after_redo"] = undo->text();
	diagnostic["redo_action_after_redo"] = redo->text();
	report();
	check(redoRestored, "one Redo restores the imported child gesture",
	      {{"diagnostic", "history-diagnostic.json"}, {"differences", diagnostic["redo_differences"]}});
	check(equal(mainState(), saved["main_before"], .06), "imported child history never changes Main");
}
void addMenu(const QString &uuid)
{
	QString error;
	QTimer::singleShot(100, qApp, [&] {
		auto *menu = window->findChild<QMenu *>("studioPortraitAddMenu");
		try {
			check(menu && menu->isVisible(), "real Portrait Add menu is visible");
			QAction *target = nullptr;
			for (auto *action : menu->actions()) {
				if (action->data().toString() == uuid) {
					target = action;
				}
			}
			check(target, "Main group is offered by the real Add menu");
			QTest::mouseClick(menu, Qt::LeftButton, {}, menu->actionGeometry(target).center());
		} catch (const std::exception &e) {
			error = e.what();
		}
		if (menu) {
			menu->close();
		}
	});
	QToolButton *add = nullptr;
	for (auto *button : window->findChildren<QToolButton *>()) {
		if (button->property("studioCommand") == "add") {
			add = button;
		}
	}
	check(add, "native Add button exists");
	get<QScrollArea>(window, "verticalPreviewOptionsScroll")->ensureWidgetVisible(add);
	add->setFocus();
	QTest::mouseClick(add, Qt::LeftButton);
	if (!error.isEmpty()) {
		throw std::runtime_error(error.toStdString());
	}
	QTest::qWait(220);
}
void pair(obs_source_t *original, obs_source_t *copy)
{
	check(original && copy && original != copy &&
		      QString::fromUtf8(obs_source_get_uuid(original)) != obs_source_get_uuid(copy),
	      "import assigns a new group UUID at every nesting level");
	OBSCanvasAutoRelease canvas = obs_source_get_canvas(copy);
	check(canvas && QString::fromUtf8(obs_canvas_get_name(canvas)) == "Vertical 9:16",
	      "cloned group belongs to Portrait canvas");
	OBSSourceAutoRelease oldFx = obs_source_get_filter_by_name(original, effectName),
			     newFx = obs_source_get_filter_by_name(copy, effectName);
	check(oldFx && newFx && oldFx.Get() != newFx.Get() &&
		      QString::fromUtf8(obs_source_get_uuid(oldFx)) != obs_source_get_uuid(newFx),
	      "group filter is cloned with an independent UUID");
	OBSDataAutoRelease oldSettings = obs_source_get_settings(oldFx), newSettings = obs_source_get_settings(newFx);
	check(equal(json(oldSettings), json(newSettings)), "cloned group filter retains its parameters");
	OBSDataAutoRelease hotkeys = obs_hotkeys_save_source(copy);
	const auto bindings = json(hotkeys);
	bool noBindings = true;
	for (auto it = bindings.begin(); it != bindings.end(); ++it) {
		noBindings = noBindings && it.value().isArray() && it.value().toArray().isEmpty();
	}
	check(noBindings, "cloned group does not duplicate Main hotkey bindings");
	const auto oldItems = items(obs_group_from_source(original)), newItems = items(obs_group_from_source(copy));
	check(oldItems.size() == newItems.size(), "group item count and order survive import");
	for (size_t i = 0; i < oldItems.size(); ++i) {
		check(obs_sceneitem_get_id(oldItems[i]) == obs_sceneitem_get_id(newItems[i]),
		      "native child item IDs and order are preserved");
		check(equal(itemState(oldItems[i]), itemState(newItems[i]), .08),
		      "import preserves local pixels, crop, flags, bounds, blending and private item state",
		      {{"original", itemState(oldItems[i])}, {"copy", itemState(newItems[i])}});
		if (obs_sceneitem_is_group(oldItems[i])) {
			pair(obs_sceneitem_get_source(oldItems[i]), obs_sceneitem_get_source(newItems[i]));
		} else {
			check(obs_sceneitem_get_source(oldItems[i]) == obs_sceneitem_get_source(newItems[i]),
			      "ordinary leaf source remains the identical instance");
		}
	}
}
void setPose(obs_sceneitem_t *item, QPointF p, bool bounded = false)
{
	obs_transform_info i{};
	i.pos = {float(p.x()), float(p.y())};
	i.scale = {1.f, 1.f};
	i.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	i.bounds_alignment = OBS_ALIGN_CENTER;
	i.bounds_type = bounded ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_NONE;
	i.bounds = {250, 180};
	obs_sceneitem_set_info2(item, &i);
	obs_sceneitem_force_update_transform(item);
}
void effect(obs_source_t *source)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_double(data, "brightness", .13);
	OBSSourceAutoRelease filter = obs_source_create_private("color_filter_v2", effectName, data);
	check(bool(filter), "local group effect is available");
	obs_source_filter_add(source, filter);
}
void bindOriginal(obs_source_t *source)
{
	OBSDataAutoRelease hotkeys = obs_hotkeys_save_source(source);
	const auto keys = json(hotkeys).keys();
	check(!keys.isEmpty(), "original group has registered local hotkeys");
	OBSDataAutoRelease key = obs_data_create();
	obs_data_set_string(key, "key", "OBS_KEY_F24");
	obs_data_set_bool(key, "control", true);
	OBSDataArrayAutoRelease bindings = obs_data_array_create();
	obs_data_array_push_back(bindings, key);
	obs_data_set_array(hotkeys, keys.front().toUtf8().constData(), bindings);
	obs_hotkeys_load_source(source, hotkeys);
	OBSDataAutoRelease effective = obs_hotkeys_save_source(source);
	check(!json(effective)[keys.front()].toArray().isEmpty(),
	      "original group owns a concrete binding before cloning");
}
void cleanup()
{
	if (saved.isEmpty()) {
		return;
	}
	if (auto *options = window->findChild<QScrollArea *>("verticalPreviewOptionsScroll")) {
		options->setMaximumHeight(optionsMaximumHeight);
	}
	QMetaObject::invokeMethod(window, "SetStudioPortraitEditing", Qt::DirectConnection, Q_ARG(bool, false));
	OBSSourceAutoRelease original = obs_get_source_by_uuid(saved["scene"].toString().toUtf8().constData());
	if (original) {
		go(original);
	}
	OBSSourceAutoRelease ownPortrait =
		obs_get_source_by_uuid(saved["portrait_uuid"].toString().toUtf8().constData());
	if (ownPortrait) {
		obs_source_remove(ownPortrait);
	}
	for (const char *name : {sceneName, groupName, nestedName, childName, siblingName, leafName}) {
		OBSSource s = source(name);
		if (s) {
			obs_source_remove(s);
		}
	}
	mode(saved["multi"].toBool());
	config_t *config = obs_frontend_get_user_config();
	if (!saved["had_multi"].toBool()) {
		config_remove_value(config, "BasicWindow", "StudioMultiCanvas");
	}
	config_save_safe(config, "tmp", nullptr);
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
void fixture()
{
	for (const char *name : {sceneName, groupName, nestedName, childName, siblingName, leafName}) {
		check(!source(name), "import namespace is unused");
	}
	OBSSourceAutoRelease main = obs_frontend_get_current_scene(),
			     preview = obs_frontend_get_current_preview_scene();
	config_t *config = obs_frontend_get_user_config();
	saved = {{"scene", obs_source_get_uuid(main)},
		 {"preview", preview ? obs_source_get_uuid(preview) : ""},
		 {"studio", obs_frontend_preview_program_mode_active()},
		 {"layout", QString::fromUtf8(window->saveState().toBase64())},
		 {"multi", !config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
				   config_get_bool(config, "BasicWindow", "StudioMultiCanvas")},
		 {"had_multi", config_has_user_value(config, "BasicWindow", "StudioMultiCanvas")}};
	write("saved.json", saved);
	obs_frontend_set_preview_program_mode(false);
	mode(true);
	OBSSceneAutoRelease scene = obs_scene_create(sceneName);
	OBSDataAutoRelease global = obs_get_private_data();
	check(!obs_data_get_bool(global, "AbsoluteCoordinates"), "relative coordinate mode is explicitly covered");
	auto *group = obs_scene_add_group2(scene, groupName, true);
	OBSScene owner = obs_sceneitem_group_get_scene(group);
	std::vector<OBSSource> colors;
	for (const char *name : {childName, siblingName, leafName}) {
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_int(data, "width", 360);
		obs_data_set_int(data, "height", 240);
		obs_data_set_int(data, "color", name == childName ? 0xff0088ff : 0xff44cc22);
		OBSSourceAutoRelease s = obs_source_create("color_source_v3", name, data, nullptr);
		check(bool(s), "local shared color exists");
		colors.emplace_back(s.Get());
	}
	setPose(obs_scene_add(owner, source(childName)), {0, 0});
	{
		auto *direct = obs_scene_find_source(owner, childName);
		obs_transform_info pose{};
		obs_sceneitem_get_info2(direct, &pose);
		pose.alignment = OBS_ALIGN_CENTER;
		pose.pos = {180, 150};
		pose.scale = {-.9f, 1.1f};
		pose.rot = 13;
		obs_sceneitem_set_info2(direct, &pose);
		OBSDataAutoRelease data = obs_sceneitem_get_private_settings(direct);
		obs_data_set_string(data, "qa_marker", "center-mirror-rotate");
	}
	auto *sibling = obs_scene_add(owner, source(siblingName));
	setPose(sibling, {420, 300}, true);
	obs_sceneitem_set_locked(sibling, true);
	obs_sceneitem_set_visible(sibling, false);
	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(sibling);
	obs_data_set_string(priv, "qa_marker", "preserve-locked-hidden");
	obs_sceneitem_set_scale_filter(sibling, OBS_SCALE_LANCZOS);
	obs_sceneitem_set_blending_mode(sibling, OBS_BLEND_ADDITIVE);
	auto *nested = obs_scene_add_group2(owner, nestedName, true);
	setPose(obs_scene_add(obs_sceneitem_group_get_scene(nested), source(leafName)), {0, 0});
	QTest::qWait(250);
	setPose(nested, {440, 30});
	setPose(group, {120, 180});
	obs_sceneitem_crop c{5, 8, 7, 9};
	obs_sceneitem_set_crop(obs_scene_find_source(owner, childName), &c);
	effect(obs_sceneitem_get_source(group));
	effect(obs_sceneitem_get_source(nested));
	bindOriginal(obs_sceneitem_get_source(group));
	bindOriginal(obs_sceneitem_get_source(nested));
	QTest::qWait(250);
	go(obs_scene_get_source(scene));
	saved["portrait_uuid"] = obs_source_get_uuid(obs_scene_get_source(portrait()));
	for (const auto &item : items(portrait())) {
		obs_sceneitem_set_visible(item, false);
	}
	saved["main_before"] = mainState();
	write("saved.json", saved);
}
void run()
{
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active() && !obs_frontend_virtualcam_active(),
		      "imports start with idle outputs");
		QFile file(root + "/saved.json");
		if (file.open(QIODevice::ReadOnly)) {
			saved = QJsonDocument::fromJson(file.readAll()).object();
		}
		if (!restoring && !saved.isEmpty()) {
			cleanup();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			saved = {};
		}
		if (restoring) {
			check(!saved.isEmpty(), "private restart baseline exists");
			mode(true);
			go(source(sceneName));
		} else {
			fixture();
		}
		dock = get<QDockWidget>(window, "verticalPreviewDock");
		dock->setFloating(true);
		dock->show();
		const QRect available = dock->screen()->availableGeometry();
		dock->resize(std::min(600, available.width() - 60), std::min(1000, available.height() - 60));
		dock->raise();
		display = get<QWidget>(window, "verticalPreviewDisplay");
		auto *options = get<QScrollArea>(window, "verticalPreviewOptionsScroll");
		optionsMaximumHeight = options->maximumHeight();
		options->setMaximumHeight(140);
		check(wait([] { return display->isVisible() && display->width() >= 180 && display->height() >= 320; }),
		      "native preview geometry is usable");
		ready();
		get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
		if (restoring) {
			check(imported() && equal(cloneState(), saved["clone_final"], .3),
			      "restart preserves imported UUIDs, fields, filters and edited child geometry");
			check(equal(mainState(), saved["main_before"], .06),
			      "restart preserves the unmodified Main source graph");
			OBSSceneItem c = child(childName);
			check(bool(c), "restart child resolves within its imported owner");
			select(c);
			cleanup();
		} else {
			addMenu(QString::fromUtf8(obs_source_get_uuid(source(groupName))));
			for (const auto &item : items(portrait())) {
				if (obs_sceneitem_is_group(item)) {
					saved["clone_uuid"] = obs_source_get_uuid(obs_sceneitem_get_source(item));
				}
			}
			check(imported(), "real Add menu creates an independent group on Portrait");
			{
				OBSSceneItem group = imported();
				pair(source(groupName), obs_sceneitem_get_source(group));
			}
			check(equal(mainState(), saved["main_before"], .06),
			      "import itself does not reload or modify Main");
			saved["clone_initial"] = cloneState();
			write("saved.json", saved);
			get<QAction>(window, "actionMainUndo")->trigger();
			check(wait([] { return !imported(); }), "structural Undo removes the imported root");
			check(wait([] {
				      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
				      OBSSourceAutoRelease s = obs_get_source_by_uuid(
					      saved["clone_uuid"].toString().toUtf8().constData());
				      return !s;
			      }),
			      "imported group actually disappears before reconstruction Redo");
			get<QAction>(window, "actionMainRedo")->trigger();
			check(wait([] { return imported() && equal(cloneState(), saved["clone_initial"], .3); }),
			      "structural Redo reconstructs the same clone UUIDs and complete fields");
			{
				OBSSceneItem group = imported();
				pair(source(groupName), obs_sceneitem_get_source(group));
			}
			select(child(childName));
			check(wait([] {
				      return display->property("studioPortraitGroupScope").toString() ==
					     QString::fromUtf8(
						     obs_source_get_uuid(obs_scene_get_source(importedScene())));
			      }),
			      "native tree activates imported child owner scope");
			auto before = cloneState();
			const auto from = point(child(childName), imported(), {.5, .5});
			gesture(from, from + QPointF(23, 29));
			history(before, cloneState());
			select(child(childName));
			before = cloneState();
			const auto edge = point(child(childName), imported(), {0, .5});
			const auto end = point(child(childName), imported(), {.12, .5});
			gesture(edge, end, Qt::AltModifier);
			history(before, cloneState());
			obs_sceneitem_crop actual{};
			obs_sceneitem_get_crop(child(childName), &actual);
			check(actual.left > 5, "imported child crop changes actual source pixels");
			// Simulate an old collection containing a shared group reference.
			{
				OBSSceneItem legacy = obs_scene_add(portrait(), source(groupName));
				setPose(legacy, {180, 450});
				select(legacy);
				const auto at = point(legacy, nullptr, {.5, .5});
				mouse(QEvent::MouseButtonDblClick, at);
				mouse(QEvent::MouseButtonRelease, at);
				check(display->property("studioPortraitGestureStatus").toString() == "shared_group",
				      "legacy shared group entry reports its safety guard");
				OBSSceneItem legacyChild =
					obs_scene_find_source(obs_sceneitem_group_get_scene(legacy), childName);
				select(legacyChild);
				check(wait([] { return !get<QDoubleSpinBox>(window, "studioPositionX")->isEnabled(); }),
				      "numeric inspector blocks a child shared with Main");
				const auto action = get<QAction>(window, "actionMainUndo")->text();
				const auto atChild = point(legacyChild, legacy, {.5, .5});
				gesture(atChild, atChild + QPointF(30, 20));
				check(equal(mainState(), saved["main_before"], .06) &&
					      get<QAction>(window, "actionMainUndo")->text() == action,
				      "legacy child drag cannot mutate Main or consume history");
				obs_sceneitem_remove(legacy);
			}
			QTest::qWait(220);
			select(child(childName));
			saved["clone_final"] = cloneState();
			write("saved.json", saved);
			obs_frontend_save();
		}
	} catch (const std::exception &e) {
		error = e.what();
		try {
			cleanup();
		} catch (...) {
		}
	}
	write(restoring ? "restore.json" : "runtime.json",
	      {{"passed", error.isEmpty()},
	       {"error", error},
	       {"checks", checks},
	       {"scope",
		"Native local Main group import through the real Qt Add menu; recursive identities, local pixel fields, leaf sharing, structural/gesture history, legacy safety guard and full restart. No native Windows, physical mouse, device, GPU-pixel or concurrent-output certification."}});
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(1800, qApp, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_PORTRAIT_IMPORTS_ROOT");
	restoring = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_IMPORTS_RESTORE");
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
