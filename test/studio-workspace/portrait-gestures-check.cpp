/* Opt-in native Qt gesture regression. All sources/scenes are disposable;
 * RESTORE=1 verifies the same saved item/scene after a complete process restart. */
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
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <stdexcept>
OBS_DECLARE_MODULE()
namespace {
QString root;
QMainWindow *window;
QWidget *display;
QDockWidget *dock;
QJsonObject saved;
QJsonArray checks;
bool restore;
template<class T> T *get(QObject *parent, const char *name)
{
	auto *value = parent->findChild<T *>(name);
	if (!value) {
		throw std::runtime_error(name);
	}
	return value;
}
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}});
	blog(LOG_INFO, "[portrait-gestures-check] %s %s", pass ? "PASS" : "FAIL", name);
	if (!pass) {
		throw std::runtime_error(name);
	}
}
void write(const char *name, QJsonObject object)
{
	QFile file(root + "/" + name);
	if (!file.open(QIODevice::WriteOnly)) {
		throw std::runtime_error("report unavailable");
	}
	file.write(QJsonDocument(object).toJson());
}
void mode(bool multi)
{
	bool accepted = false;
	check(QMetaObject::invokeMethod(window, "SetStudioCanvasMode", Qt::DirectConnection,
					Q_RETURN_ARG(bool, accepted), Q_ARG(bool, multi)) &&
		      accepted,
	      "mode action succeeds");
	QTest::qWait(120);
}
OBSScene portrait()
{
	OBSCanvasAutoRelease canvas = obs_get_canvas_by_name("Vertical 9:16");
	OBSSourceAutoRelease source = canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
	return source ? obs_scene_from_source(source) : nullptr;
}
OBSSceneItem layer()
{
	return obs_scene_find_source(portrait(), "Portrait Gestures QA shared");
}
obs_transform_info info(obs_sceneitem_t *item)
{
	obs_transform_info i{};
	obs_sceneitem_get_info2(item, &i);
	return i;
}
obs_sceneitem_crop crop(obs_sceneitem_t *item)
{
	obs_sceneitem_crop c{};
	obs_sceneitem_get_crop(item, &c);
	return c;
}
QJsonObject state(obs_sceneitem_t *item)
{
	auto i = info(item);
	auto c = crop(item);
	return {{"x", i.pos.x},
		{"y", i.pos.y},
		{"sx", i.scale.x},
		{"sy", i.scale.y},
		{"rot", i.rot},
		{"alignment", int(i.alignment)},
		{"bounds_type", int(i.bounds_type)},
		{"bw", i.bounds.x},
		{"bh", i.bounds.y},
		{"left", c.left},
		{"right", c.right},
		{"top", c.top},
		{"bottom", c.bottom}};
}
bool equal(QJsonObject a, QJsonObject b, double epsilon = .03)
{
	for (auto key : a.keys()) {
		if (std::abs(a[key].toDouble() - b[key].toDouble()) > epsilon) {
			return false;
		}
	}
	return true;
}
QPointF map(matrix4 matrix, QPointF point)
{
	vec3 a{}, b{};
	vec3_set(&a, point.x(), point.y(), 0);
	vec3_transform(&b, &a, &matrix);
	return {b.x, b.y};
}
QPointF boxPoint(obs_sceneitem_t *item, QPointF point)
{
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	return map(box, point);
}
QPointF pixel(QPointF canvas)
{
	double dpr = display->devicePixelRatioF();
	int w = qRound(display->width() * dpr), h = qRound(display->height() * dpr);
	double scale = std::min(w / 1080., h / 1920.);
	int fw = int(1080 * scale), fh = int(1920 * scale);
	return {((w - fw) / 2 + canvas.x() * fw / 1080.) / dpr, ((h - fh) / 2 + canvas.y() * fh / 1920.) / dpr};
}
void mouse(QEvent::Type type, QPointF canvas, Qt::KeyboardModifiers mods = {})
{
	// sendEvent bypasses the compositor's activation that a physical click
	// supplies. Establish the Qt focus precondition explicitly for this probe.
	if (type == QEvent::MouseButtonPress) {
		QApplication::setActiveWindow(display->window());
		display->setFocus(Qt::MouseFocusReason);
	}
	QPointF p = pixel(canvas);
	QMouseEvent event(type, p, p, display->mapToGlobal(p),
			  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
			  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
	QApplication::sendEvent(display, &event);
	QTest::qWait(40);
}
void gesture(QPointF from, QPointF to, Qt::KeyboardModifiers mods = {})
{
	mouse(QEvent::MouseButtonPress, from, mods);
	for (int n = 1; n <= 5; ++n) {
		mouse(QEvent::MouseMove, from + (to - from) * n / 5., mods);
	}
	mouse(QEvent::MouseButtonRelease, to, mods);
	QTest::qWait(250);
}
void select()
{
	auto *combo = get<QComboBox>(window, "studioActiveCanvas");
	combo->setCurrentIndex(1);
	QTest::qWait(100);
	auto *tree = get<QListView>(window, "sources");
	for (int n = 0; n < tree->model()->rowCount(); ++n) {
		auto idx = tree->model()->index(n, 0);
		if (idx.data(Qt::AccessibleTextRole).toString() == "Portrait Gestures QA shared") {
			tree->scrollTo(idx);
			QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(idx).center());
			QTest::qWait(100);
			return;
		}
	}
	throw std::runtime_error("fixture not in source tree");
}
void reset(float rotation = 0, float sx = 1, uint32_t align = OBS_ALIGN_TOP | OBS_ALIGN_LEFT,
	   obs_bounds_type bounds = OBS_BOUNDS_NONE)
{
	auto item = layer();
	obs_transform_info i{};
	i.pos = {360, 650};
	i.scale = {sx, 1};
	i.rot = rotation;
	i.alignment = align;
	i.bounds_type = bounds;
	i.bounds = {360, 480};
	i.bounds_alignment = OBS_ALIGN_CENTER;
	obs_sceneitem_crop c{};
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &i);
	obs_sceneitem_set_crop(item, &c);
	obs_sceneitem_defer_update_end(item);
	select();
}
void undoRedo(QJsonObject before, QJsonObject after)
{
	get<QAction>(window, "actionMainUndo")->trigger();
	QTest::qWait(180);
	check(equal(state(layer()), before), "one Undo restores the complete gesture including crop");
	get<QAction>(window, "actionMainRedo")->trigger();
	QTest::qWait(180);
	check(equal(state(layer()), after), "one Redo reapplies the complete gesture");
}
void cleanup()
{
	if (saved.isEmpty()) {
		return;
	}
	QMetaObject::invokeMethod(window, "SetStudioPortraitEditing", Qt::DirectConnection, Q_ARG(bool, false));
	OBSSourceAutoRelease original = obs_get_source_by_uuid(saved["scene"].toString().toUtf8().constData());
	if (original) {
		obs_frontend_set_current_scene(original);
	}
	for (const char *name :
	     {"Portrait Gestures QA", "Portrait Gestures QA shared", "Portrait Gestures QA blocker",
	      "Portrait Gestures QA group", "Portrait Gestures QA pair", "Portrait Gestures QA isolation group",
	      "Portrait Gestures QA isolation locked", "Portrait Gestures QA isolation child"}) {
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
void run()
{
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active(), "gesture fixture begins with outputs idle");
		if (restore) {
			QFile file(root + "/saved.json");
			check(file.open(QIODevice::ReadOnly), "private restart baseline exists");
			saved = QJsonDocument::fromJson(file.readAll()).object();
		} else {
			OBSSourceAutoRelease original = obs_frontend_get_current_scene(),
					     preview = obs_frontend_get_current_preview_scene();
			auto *u = obs_frontend_get_user_config();
			saved = {{"scene", obs_source_get_uuid(original)},
				 {"preview", preview ? obs_source_get_uuid(preview) : ""},
				 {"multi", !config_has_user_value(u, "BasicWindow", "StudioMultiCanvas") ||
						   config_get_bool(u, "BasicWindow", "StudioMultiCanvas")},
				 {"had_multi", config_has_user_value(u, "BasicWindow", "StudioMultiCanvas")},
				 {"studio", obs_frontend_preview_program_mode_active()},
				 {"layout", QString::fromUtf8(window->saveState().toBase64())}};
			write("saved.json", saved);
			obs_frontend_set_preview_program_mode(false);
			mode(true);
			OBSSceneAutoRelease fixture = obs_scene_create("Portrait Gestures QA");
			OBSDataAutoRelease data = obs_data_create();
			obs_data_set_int(data, "width", 360);
			obs_data_set_int(data, "height", 480);
			obs_data_set_int(data, "color", 0xff0099ee);
			OBSSourceAutoRelease source =
				obs_source_create("color_source_v3", "Portrait Gestures QA shared", data, nullptr);
			auto *main = obs_scene_add(fixture, source);
			vec2 p{70, 90};
			obs_sceneitem_set_pos(main, &p);
			obs_frontend_set_current_scene(obs_scene_get_source(fixture));
			QTest::qWait(350);
			check(bool(portrait()), "fixture has its own linked portrait scene");
			obs_scene_add(portrait(), source);
			obs_scene_enum_items(
				portrait(),
				[](obs_scene_t *, obs_sceneitem_t *item, void *) {
					OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
					if (obs_data_get_bool(settings, "otaku_mirror")) {
						obs_sceneitem_set_visible(item, false);
					}
					return true;
				},
				nullptr);
		}
		mode(true);
		dock = get<QDockWidget>(window, "verticalPreviewDock");
		dock->setFloating(true);
		dock->show();
		dock->resize(520, 1200);
		dock->raise();
		display = get<QWidget>(window, "verticalPreviewDisplay");
		QTest::qWait(350);
		check(display->isVisible() && display->width() >= 180 && display->height() >= 320,
		      "native portrait display is exposed for real Qt mouse events");

		auto *cropControl = get<QToolButton>(window, "studioPortraitCropMode");
		QRect displayRect(display->mapTo(dock, QPoint()), display->size());
		QRect cropRect(cropControl->mapTo(dock, QPoint()), cropControl->size());
		check(cropControl->isVisible() && dock->rect().contains(cropRect) && !displayRect.intersects(cropRect),
		      "Crop control stays inside the dock and outside the native preview even on HiDPI");
		OBSSceneItem item = layer();
		check(bool(item), "saved portrait fixture item is available");
		OBSSourceAutoRelease mainSource = obs_get_source_by_name("Portrait Gestures QA");
		auto *mainItem =
			obs_scene_find_source(obs_scene_from_source(mainSource), "Portrait Gestures QA shared");
		auto mainBefore = state(mainItem);
		if (restore) {
			check(saved["portrait_uuid"].toString() ==
				      obs_source_get_uuid(obs_scene_get_source(portrait())),
			      "restart reuses the same linked portrait scene UUID");
			check(saved["item_id"].toVariant().toLongLong() == obs_sceneitem_get_id(item),
			      "restart retains the edited scene item ID");
			check(equal(state(item), saved["final"].toObject()),
			      "move resize and crop all survive full process restart");
			check(info(mainItem).pos.x == 70 && info(mainItem).pos.y == 90 && crop(mainItem).left == 0,
			      "Main transform and crop remain original after restart");
			OBSSceneItem pair = obs_scene_find_source(portrait(), "Portrait Gestures QA pair");
			check(pair && equal(state(pair), saved["pair_final"].toObject()) &&
				      saved["pair_id"].toVariant().toLongLong() == obs_sceneitem_get_id(pair) &&
				      saved["pair_uuid"].toString() ==
					      obs_source_get_uuid(obs_sceneitem_get_source(pair)),
			      "second multi-drag participant retains its transform item ID and source UUID after restart");
			select();
			QTest::qWait(300);
			check(std::abs(get<QDoubleSpinBox>(window, "studioPositionX")->value() - info(item).pos.x) <
				      .11,
			      "inspector reloads the persisted gesture transform");
			cleanup();
		} else {
			reset();
			// Start in Main: a body click must switch editor, selection and inspector.
			get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(0);
			const auto before = state(item);
			QPointF center = boxPoint(item, {.5, .5});
			gesture(center, center + QPointF(120, 180));
			check(window->property("studioEditingCanvas").toString() == "portrait" &&
				      obs_sceneitem_selected(item),
			      "direct click activates Portrait and selects its native layer");
			check(get<QLabel>(window, "studioSelectedSourceName")->text() == "Portrait Gestures QA shared",
			      "inspector follows direct preview selection");
			auto *auxiliaryLayers = get<QListWidget>(window, "verticalPreviewLayerList");
			check(auxiliaryLayers->currentItem() &&
				      auxiliaryLayers->currentItem()->data(Qt::UserRole).toLongLong() ==
					      obs_sceneitem_get_id(item),
			      "portrait dock layer list highlights the same item as preview and native source tree");
			check(std::abs(info(item).pos.x - 480) < .1 && std::abs(info(item).pos.y - 830) < .1,
			      "multi-step drag maps logical preview coordinates to canvas pixels");
			check(std::abs(get<QDoubleSpinBox>(window, "studioPositionX")->value() - 480) < .11,
			      "inspector updates during direct editing");
			undoRedo(before, state(item));
			// Every corner and side, including opposite-anchor preservation.
			const QPointF hs[] = {{0, 0}, {.5, 0}, {1, 0}, {1, .5}, {1, 1}, {.5, 1}, {0, 1}, {0, .5}};
			for (const QPointF h : hs) {
				reset();
				auto startState = state(item);
				QPointF from = boxPoint(item, h), anchor = boxPoint(item, {1 - h.x(), 1 - h.y()});
				QPointF delta((h.x() - .5) * 120, (h.y() - .5) * 160);
				gesture(from, from + delta);
				auto after = state(item);
				check(info(item).scale.x > 1.1 &&
					      std::abs(info(item).scale.x - info(item).scale.y) < .001,
				      "corner or edge handle enlarges while preserving aspect ratio");
				check(QLineF(anchor, boxPoint(item, {1 - h.x(), 1 - h.y()})).length() < 1.5,
				      "resize keeps the opposite anchor stable");
				undoRedo(startState, after);
			}
			reset();
			auto s = state(item);
			QPointF from = boxPoint(item, {1, 1});
			gesture(from, from + QPointF(180, 30), Qt::ShiftModifier);
			check(std::abs(info(item).scale.x - 1.5) < .01 && std::abs(info(item).scale.y - 1.0625) < .01,
			      "Shift handle supports free aspect ratio");
			undoRedo(s, state(item));
			reset();
			from = boxPoint(item, {.5, .5});
			gesture(from, from + QPointF(40, 140), Qt::ShiftModifier);
			check(std::abs(info(item).pos.x - 360) < .01 && std::abs(info(item).pos.y - 790) < .01,
			      "Shift drag constrains movement to the dominant axis");
			// Cropping and extending an existing crop keep the far edge fixed.
			reset();
			s = state(item);
			from = boxPoint(item, {0, .5});
			QPointF anchor = boxPoint(item, {1, .5});
			gesture(from, from + QPointF(90, 0), Qt::AltModifier);
			check(crop(item).left == 90 && std::abs(info(item).pos.x - 450) < .1,
			      "Alt edge crops source pixels and moves the crop boundary");
			check(QLineF(anchor, boxPoint(item, {1, .5})).length() < 1,
			      "crop preserves the opposite edge and original scale");
			undoRedo(s, state(item));
			from = boxPoint(item, {0, .5});
			gesture(from, from - QPointF(45, 0), Qt::AltModifier);
			check(crop(item).left == 45, "outward Alt drag restores previously cropped pixels");
			reset();
			s = state(item);
			from = boxPoint(item, {1, 1});
			gesture(from, from - QPointF(60, 100), Qt::AltModifier);
			check(crop(item).right == 60 && crop(item).bottom == 100, "Alt corner crops both axes");
			undoRedo(s, state(item));
			from = boxPoint(item, {1, .5});
			gesture(from, from - QPointF(1000, 0), Qt::AltModifier);
			check(crop(item).left + crop(item).right == 358,
			      "extreme crop retains at least two visible source pixels");
			// Rotation, mirroring and position alignment use the oriented box, not an AABB.
			for (auto aligned :
			     {uint32_t(OBS_ALIGN_CENTER), uint32_t(OBS_ALIGN_RIGHT | OBS_ALIGN_BOTTOM)}) {
				reset(27, -1, aligned);
				s = state(item);
				from = boxPoint(item, {1, 1});
				anchor = boxPoint(item, {0, 0});
				matrix4 b;
				obs_sceneitem_get_box_transform(item, &b);
				gesture(from, map(b, {1.25, 1.25}));
				blog(LOG_INFO,
				     "[portrait-gestures-check] aligned=%u from=%f,%f to=%f,%f selected=%d state=%s",
				     aligned, from.x(), from.y(), map(b, {1.25, 1.25}).x(), map(b, {1.25, 1.25}).y(),
				     obs_sceneitem_selected(item),
				     QJsonDocument(state(item)).toJson(QJsonDocument::Compact).constData());
				check(info(item).scale.x < -1.2 && info(item).rot == 27,
				      "rotated mirrored source resizes without changing sign or angle");
				check(QLineF(anchor, boxPoint(item, {0, 0})).length() < 1.5,
				      "rotated aligned resize preserves its opposite corner");
				undoRedo(s, state(item));
				s = state(item);
				from = boxPoint(item, {0, .5});
				obs_sceneitem_get_box_transform(item, &b);
				gesture(from, map(b, {.2, .5}), Qt::AltModifier);
				check(crop(item).left == 72, "rotated mirrored crop uses source-local pixels");
				undoRedo(s, state(item));
			}
			reset(0, 1, OBS_ALIGN_TOP | OBS_ALIGN_LEFT, OBS_BOUNDS_SCALE_INNER);
			s = state(item);
			from = boxPoint(item, {1, 1});
			gesture(from, from + QPointF(90, 120));
			check(std::abs(info(item).bounds.x - 450) < .1 && std::abs(info(item).bounds.y - 600) < .1 &&
				      info(item).scale.x == 1,
			      "bounded layer resizes bounds rather than corrupting source scale");
			undoRedo(s, state(item));
			s = state(item);
			from = boxPoint(item, {0, .5});
			gesture(from, from + QPointF(90, 0), Qt::AltModifier);
			check(crop(item).left == 72 && info(item).pos.x == s["x"].toDouble() &&
				      info(item).bounds.x == s["bw"].toDouble(),
			      "bounded crop follows OBS refit semantics and retains the bounds");
			undoRedo(s, state(item));
			reset();
			s = state(item);
			auto *cropToggle = get<QToolButton>(window, "studioPortraitCropMode");
			QTest::mouseClick(cropToggle, Qt::LeftButton);
			from = boxPoint(item, {0, .5});
			gesture(from, from + QPointF(60, 0));
			check(crop(item).left == 60 && cropToggle->isChecked(),
			      "Crop toggle edits directly without Alt or a modal dialog");
			undoRedo(s, state(item));
			QTest::mouseClick(cropToggle, Qt::LeftButton);
			check(!cropToggle->isChecked(), "Crop toggle returns handles to resizing");

			// Cancellation and no-op history.
			reset();
			s = state(item);
			QString action = get<QAction>(window, "actionMainUndo")->text();
			from = boxPoint(item, {.5, .5});
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(80, 100));
			QTest::keyClick(display, Qt::Key_Escape);
			mouse(QEvent::MouseButtonRelease, from + QPointF(80, 100));
			check(equal(s, state(item)) && get<QAction>(window, "actionMainUndo")->text() == action,
			      "Escape cancels a partial drag without adding Undo history");
			gesture(from, from);
			check(equal(s, state(item)) && get<QAction>(window, "actionMainUndo")->text() == action,
			      "selection-only click leaves transform and Undo history intact");
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(80, 100));
			dock->resize(dock->width() + 30, dock->height());
			QTest::qWait(100);
			mouse(QEvent::MouseButtonRelease, from + QPointF(80, 100));
			check(equal(s, state(item)),
			      "resizing the preview cancels rather than jumping a partial gesture");
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(80, 100));
			get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(0);
			QTest::qWait(100);
			mouse(QEvent::MouseButtonRelease, from + QPointF(80, 100));
			check(equal(s, state(item)), "changing editing canvas cancels a partial gesture");
			select();
			obs_sceneitem_set_locked(item, true);
			gesture(from, from + QPointF(80, 100));
			check(equal(s, state(item)), "locked layer cannot be moved through the preview");
			obs_sceneitem_set_locked(item, false);
			obs_sceneitem_set_visible(item, false);
			gesture(from, from + QPointF(80, 100));
			check(equal(s, state(item)), "hidden layer cannot be moved through the preview");
			obs_sceneitem_set_visible(item, true);
			reset();
			s = state(item);
			from = boxPoint(item, {.5, .5});
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(60, 80));
			QTest::keyClick(display, Qt::Key_Z, Qt::ControlModifier);
			check(!equal(s, state(item)), "Undo shortcut cannot race an unfinished drag");
			QTest::keyClick(display, Qt::Key_Escape);
			mouse(QEvent::MouseButtonRelease, from + QPointF(60, 80));
			check(equal(s, state(item)),
			      "Escape still restores a gesture after an intercepted Undo shortcut");
			gesture(from, from + QPointF(60, 80));
			auto keyboardAfter = state(item);
			check(!equal(s, keyboardAfter), "keyboard Undo test starts with an actually committed gesture");
			QApplication::setActiveWindow(display->window());
			display->setFocus(Qt::MouseFocusReason);
			blog(LOG_INFO,
			     "[portrait-gestures-check] keyboard focus=%d active=%d undo=%d before=%s after=%s",
			     display->hasFocus(), QApplication::activeWindow() == display->window(),
			     get<QAction>(window, "actionMainUndo")->isEnabled(),
			     QJsonDocument(s).toJson(QJsonDocument::Compact).constData(),
			     QJsonDocument(keyboardAfter).toJson(QJsonDocument::Compact).constData());
			QTest::keyClick(display, Qt::Key_Z, Qt::ControlModifier);
			QTest::qWait(200);
			check(equal(s, state(item)), "Ctrl+Z works from the focused native portrait display");
			QTest::keyClick(display, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
			QTest::qWait(200);
			check(equal(keyboardAfter, state(item)), "Ctrl+Shift+Z redoes the gesture from the display");
			reset();
			s = state(item);
			from = boxPoint(item, {.5, .5});
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(60, 80));
			OBSSourceAutoRelease original =
				obs_get_source_by_uuid(saved["scene"].toString().toUtf8().constData());
			OBSScene previousPortrait = portrait();
			obs_frontend_set_current_scene(original);
			for (int n = 0; n < 40 && portrait().Get() == previousPortrait.Get(); ++n) {
				QTest::qWait(50);
			}
			check(portrait().Get() != previousPortrait.Get(),
			      "program change reaches the linked portrait controller");
			QTest::qWait(100);
			mouse(QEvent::MouseButtonRelease, from + QPointF(60, 80));
			blog(LOG_INFO, "[portrait-gestures-check] scene cancel actual=%s before=%s",
			     QJsonDocument(state(item)).toJson(QJsonDocument::Compact).constData(),
			     QJsonDocument(s).toJson(QJsonDocument::Compact).constData());
			check(equal(s, state(item)), "switching program scene rolls back the old portrait gesture");
			obs_frontend_set_current_scene(mainSource);
			QTest::qWait(500);
			reset();
			s = state(item);
			from = boxPoint(item, {.5, .5});
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(60, 80));
			mode(false);
			check(equal(s, state(item)),
			      "disabling Multi rolls back an unfinished gesture before detaching");
			mode(true);
			mouse(QEvent::MouseButtonRelease, from + QPointF(60, 80));
			reset();
			s = state(item);
			from = boxPoint(item, {.5, .5});

			// Topmost body wins; blank canvas clears native selection.
			OBSDataAutoRelease d = obs_data_create();
			obs_data_set_int(d, "width", 360);
			obs_data_set_int(d, "height", 480);
			obs_data_set_int(d, "color", 0xff11ff00);
			OBSSourceAutoRelease blocker =
				obs_source_create("color_source_v3", "Portrait Gestures QA blocker", d, nullptr);
			OBSSceneItem top = obs_scene_add(portrait(), blocker);
			auto i = info(item);
			obs_sceneitem_set_info2(top, &i);
			QTest::qWait(250);
			gesture(from, from + QPointF(40, 50));
			blog(LOG_INFO,
			     "[portrait-gestures-check] top selected=%d shared selected=%d shared=%s expected=%s top=%s",
			     obs_sceneitem_selected(top), obs_sceneitem_selected(item),
			     QJsonDocument(state(item)).toJson(QJsonDocument::Compact).constData(),
			     QJsonDocument(s).toJson(QJsonDocument::Compact).constData(),
			     QJsonDocument(state(top)).toJson(QJsonDocument::Compact).constData());
			check(obs_sceneitem_selected(top) && equal(s, state(item)),
			      "body hit chooses the topmost visible unlocked layer");
			gesture({1050, 1850}, {1050, 1850});
			check(!obs_sceneitem_selected(top) && !obs_sceneitem_selected(item),
			      "blank canvas click clears native selection");
			check(auxiliaryLayers->currentRow() == -1,
			      "blank preview also clears the auxiliary layer highlight");
			// Removing the active item must not resurrect it or create an Undo action.
			QPointF at = boxPoint(top, {.5, .5});
			action = get<QAction>(window, "actionMainUndo")->text();
			mouse(QEvent::MouseButtonPress, at);
			mouse(QEvent::MouseMove, at + QPointF(60, 60));
			obs_sceneitem_remove(top);
			QTest::qWait(100);
			mouse(QEvent::MouseButtonRelease, at + QPointF(60, 60));
			check(!obs_sceneitem_get_scene(top) && get<QAction>(window, "actionMainUndo")->text() == action,
			      "removing an item during drag cancels without resurrecting it");
			obs_source_remove(blocker);
			// Group transforms are edited as a whole, preserving children.
			OBSSceneItem group = obs_scene_add_group(portrait(), "Portrait Gestures QA group");
			obs_sceneitem_group_add_item(group, item);
			QTest::qWait(150);
			obs_transform_info gi = info(group);
			gi.pos = {300, 600};
			obs_sceneitem_set_info2(group, &gi);
			auto child = state(item);
			auto groupBefore = state(group);
			at = boxPoint(group, {.5, .5});
			gesture(at, at + QPointF(50, 90));
			check(std::abs(info(group).pos.x - 350) < 1 && equal(child, state(item)),
			      "direct group drag moves the unit without changing child transforms");
			get<QAction>(window, "actionMainUndo")->trigger();
			QTest::qWait(200);
			check(equal(groupBefore, state(group)), "group drag Undo restores the group transform");
			obs_sceneitem_group_remove_item(group, item);
			obs_sceneitem_remove(group);
			QTest::qWait(200);
			// Multiple top-level selections move as one transaction. Use disjoint
			// geometry so selection tests cannot accidentally hit the other body.
			reset();
			OBSDataAutoRelease pairData = obs_data_create();
			obs_data_set_int(pairData, "width", 220);
			obs_data_set_int(pairData, "height", 250);
			obs_data_set_int(pairData, "color", 0xffff5511);
			OBSSourceAutoRelease pairSource =
				obs_source_create("color_source_v3", "Portrait Gestures QA pair", pairData, nullptr);
			auto *pairMain = obs_scene_add(obs_scene_from_source(mainSource), pairSource);
			vec2 mainPosition{15, 25};
			obs_sceneitem_set_pos(pairMain, &mainPosition);
			OBSSceneItem pair = obs_scene_add(portrait(), pairSource);
			vec2 pairPosition{80, 1250};
			obs_sceneitem_set_pos(pair, &pairPosition);
			QTest::qWait(250);
			auto pairMainBefore = state(pairMain);
			auto click = [&](obs_sceneitem_t *target, Qt::KeyboardModifiers modifiers = {}) {
				const auto center = boxPoint(target, {.5, .5});
				gesture(center, center, modifiers);
			};
			auto bothSelected = [&] {
				return obs_sceneitem_selected(item) && obs_sceneitem_selected(pair) &&
				       get<QListView>(window, "sources")->selectionModel()->selectedRows().size() == 2;
			};
			action = get<QAction>(window, "actionMainUndo")->text();
			click(pair, Qt::ControlModifier);
			check(bothSelected(), "Ctrl click adds another preview layer to the native tree selection");
			check(get<QAction>(window, "actionMainUndo")->text() == action,
			      "multiple selection alone does not consume Undo history");
			click(pair, Qt::ControlModifier);
			check(obs_sceneitem_selected(item) && !obs_sceneitem_selected(pair),
			      "Ctrl click toggles a selected member off without selecting it again on release");
			gesture({1040, 1850}, {1040, 1850}, Qt::ControlModifier);
			check(obs_sceneitem_selected(item), "Ctrl blank click preserves the existing selection");
			click(pair, Qt::ControlModifier);
			click(item);
			check(bothSelected(), "plain click on a selected body preserves multiple selection");
			auto firstBefore = state(item), pairBefore = state(pair);
			from = boxPoint(item, {.5, .5});
			gesture(from, from + QPointF(75, 95));
			check(std::abs(info(item).pos.x - firstBefore["x"].toDouble() - 75) < .1 &&
				      std::abs(info(item).pos.y - firstBefore["y"].toDouble() - 95) < .1 &&
				      std::abs(info(pair).pos.x - pairBefore["x"].toDouble() - 75) < .1 &&
				      std::abs(info(pair).pos.y - pairBefore["y"].toDouble() - 95) < .1,
			      "one body drag translates every selected layer by the same canvas delta");
			auto firstAfter = state(item), pairAfter = state(pair);
			get<QAction>(window, "actionMainUndo")->trigger();
			QTest::qWait(150);
			check(equal(firstBefore, state(item)) && equal(pairBefore, state(pair)),
			      "one Undo restores all participants of the multi-layer drag");
			get<QAction>(window, "actionMainRedo")->trigger();
			QTest::qWait(150);
			check(equal(firstAfter, state(item)) && equal(pairAfter, state(pair)),
			      "one Redo reapplies all participants of the multi-layer drag");
			firstBefore = state(item);
			pairBefore = state(pair);
			from = boxPoint(pair, {.5, .5});
			gesture(from, from + QPointF(90, 140), Qt::ShiftModifier);
			check(std::abs(info(item).pos.x - firstBefore["x"].toDouble()) < .1 &&
				      std::abs(info(pair).pos.x - pairBefore["x"].toDouble()) < .1 &&
				      std::abs(info(item).pos.y - firstBefore["y"].toDouble() - 140) < .1 &&
				      std::abs(info(pair).pos.y - pairBefore["y"].toDouble() - 140) < .1,
			      "Shift constrains the complete multiple selection to the dominant axis");
			firstBefore = state(item);
			pairBefore = state(pair);
			from = boxPoint(item, {.8, .8});
			gesture(from, from + QPointF(30, 40));
			check(info(item).scale.x == firstBefore["sx"].toDouble() &&
				      info(pair).scale.x == pairBefore["sx"].toDouble() && bothSelected() &&
				      std::abs(info(item).pos.x - firstBefore["x"].toDouble() - 30) < .1 &&
				      std::abs(info(pair).pos.y - pairBefore["y"].toDouble() - 40) < .1,
			      "multi-selection body away from collective handles translates all members without resizing");
			firstBefore = state(item);
			pairBefore = state(pair);
			action = get<QAction>(window, "actionMainUndo")->text();
			from = boxPoint(item, {.5, .5});
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(70, 50));
			QTest::keyClick(display, Qt::Key_Escape);
			mouse(QEvent::MouseButtonRelease, from + QPointF(70, 50));
			check(equal(firstBefore, state(item)) && equal(pairBefore, state(pair)) &&
				      get<QAction>(window, "actionMainUndo")->text() == action,
			      "Escape rolls back every participant without creating an Undo entry");
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(70, 50));
			obs_sceneitem_set_locked(pair, true);
			QTest::qWait(100);
			mouse(QEvent::MouseButtonRelease, from + QPointF(70, 50));
			check(equal(firstBefore, state(item)) && equal(pairBefore, state(pair)) &&
				      obs_sceneitem_locked(pair),
			      "locking any participant mid-drag rolls back the whole gesture and preserves its lock");
			obs_sceneitem_set_locked(pair, false);
			mouse(QEvent::MouseButtonPress, from);
			mouse(QEvent::MouseMove, from + QPointF(70, 50));
			obs_sceneitem_remove(pair);
			QTest::qWait(100);
			mouse(QEvent::MouseButtonRelease, from + QPointF(70, 50));
			check(equal(firstBefore, state(item)) && !obs_sceneitem_get_scene(pair),
			      "removing a member cancels all surviving members without resurrecting the removed item");
			pair = obs_scene_add(portrait(), pairSource);
			obs_sceneitem_set_pos(pair, &pairPosition);
			QTest::qWait(200);
			reset(27, -1, OBS_ALIGN_CENTER);
			click(pair, Qt::ControlModifier);
			firstBefore = state(item);
			pairBefore = state(pair);
			from = boxPoint(item, {.5, .5});
			gesture(from, from + QPointF(80, 90));
			check(info(item).rot == 27 && info(item).scale.x == -1 &&
				      std::abs(info(pair).pos.x - pairBefore["x"].toDouble() - 80) < .1,
			      "rotated mirrored participants translate without altering rotation or scale");
			check(equal(pairMainBefore, state(pairMain)),
			      "multi-selection gestures leave the second shared Main item untouched");
			// A selected locked root and a selected group child are deliberately
			// outside the top-level editable gesture. Their later external edits
			// must not be captured and overwritten by its transform-only history.
			{
				reset();
				obs_sceneitem_set_pos(pair, &pairPosition);
				OBSSourceAutoRelease lockedSource = obs_source_create(
					"color_source_v3", "Portrait Gestures QA isolation locked", pairData, nullptr);
				OBSSourceAutoRelease childSource = obs_source_create(
					"color_source_v3", "Portrait Gestures QA isolation child", pairData, nullptr);
				OBSSceneItem locked = obs_scene_add(portrait(), lockedSource);
				vec2 lockedPosition{780, 150};
				obs_sceneitem_set_pos(locked, &lockedPosition);
				obs_sceneitem_set_locked(locked, true);
				OBSSceneItem isolationGroup =
					obs_scene_add_group2(portrait(), "Portrait Gestures QA isolation group", true);
				OBSSource isolationGroupSource = obs_sceneitem_get_source(isolationGroup);
				OBSSceneItem isolationChild =
					obs_scene_add(obs_sceneitem_group_get_scene(isolationGroup), childSource);
				QTest::qWait(200);
				vec2 groupPosition{780, 1500};
				obs_sceneitem_set_pos(isolationGroup, &groupPosition);
				auto *tree = get<QListView>(window, "sources");
				check(QMetaObject::invokeMethod(tree, "RefreshItems", Qt::DirectConnection),
				      "isolation fixture refreshes the native tree including the group child");
				click(pair, Qt::ControlModifier);
				auto selectNamed = [tree](const char *name) {
					for (int attempt = 0; attempt < 20; ++attempt) {
						for (int row = 0; row < tree->model()->rowCount(); ++row) {
							auto index = tree->model()->index(row, 0);
							if (index.data(Qt::AccessibleTextRole).toString() == name) {
								tree->selectionModel()->select(
									index, QItemSelectionModel::Select);
								return;
							}
						}
						QTest::qWait(50);
					}
					throw std::runtime_error("isolation fixture item not in native source tree");
				};
				selectNamed("Portrait Gestures QA isolation locked");
				selectNamed("Portrait Gestures QA isolation child");
				check(obs_sceneitem_selected(item) && obs_sceneitem_selected(pair) &&
					      obs_sceneitem_selected(locked) &&
					      obs_sceneitem_selected(isolationChild) &&
					      !obs_sceneitem_selected(isolationGroup) &&
					      tree->selectionModel()->selectedRows().size() == 4,
				      "gesture isolation begins with two editable roots a locked root and a group child selected");
				auto firstIsolatedBefore = state(item), pairIsolatedBefore = state(pair);
				auto lockedBefore = state(locked), childBefore = state(isolationChild);
				from = boxPoint(item, {.5, .5});
				gesture(from, from + QPointF(55, 65));
				auto firstIsolatedAfter = state(item), pairIsolatedAfter = state(pair);
				check(std::abs(info(item).pos.x - firstIsolatedBefore["x"].toDouble() - 55) < .1 &&
					      std::abs(info(pair).pos.y - pairIsolatedBefore["y"].toDouble() - 65) <
						      .1 &&
					      equal(lockedBefore, state(locked)) &&
					      equal(childBefore, state(isolationChild)),
				      "multiple drag moves its editable roots while leaving selected exclusions unchanged");
				auto lockedInfo = info(locked), childInfo = info(isolationChild);
				lockedInfo.pos.x += 33;
				lockedInfo.pos.y += 44;
				lockedInfo.rot = 11;
				childInfo.pos.x += 19;
				childInfo.pos.y += 23;
				childInfo.rot = 17;
				obs_sceneitem_crop lockedCrop{}, childCrop{};
				lockedCrop.left = 11;
				lockedCrop.top = 7;
				childCrop.left = 9;
				childCrop.bottom = 5;
				obs_sceneitem_defer_update_begin(locked);
				obs_sceneitem_set_info2(locked, &lockedInfo);
				obs_sceneitem_set_crop(locked, &lockedCrop);
				obs_sceneitem_defer_update_end(locked);
				obs_sceneitem_defer_update_begin(isolationChild);
				obs_sceneitem_set_info2(isolationChild, &childInfo);
				obs_sceneitem_set_crop(isolationChild, &childCrop);
				obs_sceneitem_defer_update_end(isolationChild);
				// Group auto-sizing can normalize the child's local position. Capture
				// its settled pose rather than asserting the requested coordinates.
				QTest::qWait(200);
				auto lockedExternal = state(locked), childExternal = state(isolationChild);
				check(!equal(lockedBefore, lockedExternal) && !equal(childBefore, childExternal) &&
					      info(locked).rot == 11 && info(isolationChild).rot == 17 &&
					      crop(locked).left == 11 && crop(isolationChild).left == 9,
				      "excluded selected items receive distinct external pose and crop changes after the gesture");
				get<QAction>(window, "actionMainUndo")->trigger();
				QTest::qWait(200);
				check(equal(firstIsolatedBefore, state(item)) &&
					      equal(pairIsolatedBefore, state(pair)) &&
					      equal(lockedExternal, state(locked)) &&
					      equal(childExternal, state(isolationChild)),
				      "gesture Undo restores only participants and preserves external locked root and child edits");
				get<QAction>(window, "actionMainRedo")->trigger();
				QTest::qWait(200);
				check(equal(firstIsolatedAfter, state(item)) && equal(pairIsolatedAfter, state(pair)) &&
					      equal(lockedExternal, state(locked)) &&
					      equal(childExternal, state(isolationChild)),
				      "gesture Redo preserves external pose and crop changes on both selected exclusions");
				obs_sceneitem_remove(locked);
				obs_sceneitem_remove(isolationGroup);
				obs_source_remove(isolationGroupSource);
				obs_source_remove(lockedSource);
				obs_source_remove(childSource);
				QTest::qWait(200);
			}
			// User layouts may tab the preview together with its companion docks.
			// Selecting a layer must not raise one of those tabs over the gesture.
			{
				reset();
				const QByteArray gestureLayout = window->saveState();
				auto *inspector = get<QDockWidget>(window, "studioInspectorDock");
				auto *sources = get<QDockWidget>(window, "sourcesDock");
				dock->setFloating(false);
				inspector->show();
				sources->show();
				window->tabifyDockWidget(inspector, dock);
				window->tabifyDockWidget(dock, sources);
				dock->show();
				dock->raise();
				QTest::qWait(250);
				check(window->tabifiedDockWidgets(dock).contains(inspector) &&
					      window->tabifiedDockWidgets(dock).contains(sources) &&
					      !dock->visibleRegion().isEmpty() && !display->visibleRegion().isEmpty(),
				      "custom layout exposes the preview in the same tab group as inspector and sources");
				auto tabbedBefore = state(item);
				from = boxPoint(item, {.5, .5});
				mouse(QEvent::MouseButtonPress, from);
				mouse(QEvent::MouseMove, from + QPointF(35, 45));
				check(!dock->visibleRegion().isEmpty() && !display->visibleRegion().isEmpty(),
				      "portrait selection keeps the tabbed preview exposed during the gesture");
				mouse(QEvent::MouseButtonRelease, from + QPointF(35, 45));
				QTest::qWait(200);
				check(std::abs(info(item).pos.x - tabbedBefore["x"].toDouble() - 35) < .1 &&
					      std::abs(info(item).pos.y - tabbedBefore["y"].toDouble() - 45) < .1 &&
					      !dock->visibleRegion().isEmpty(),
				      "drag completes at the correct canvas position with companion docks tabified");
				get<QAction>(window, "actionMainUndo")->trigger();
				QTest::qWait(200);
				check(equal(tabbedBefore, state(item)),
				      "native Undo restores the gesture from the custom tabbed layout");
				check(window->restoreState(gestureLayout),
				      "custom layout test restores the preceding dock state");
				dock->show();
				dock->raise();
				QTest::qWait(250);
				check(dock->isFloating() && !display->visibleRegion().isEmpty(),
				      "floating gesture preview is restored before the persisted composition sequence");
			}
			// Persist a combined move/resize/crop for stage two.
			reset();
			from = boxPoint(item, {.5, .5});
			gesture(from, from + QPointF(60, 80));
			from = boxPoint(item, {1, 1});
			gesture(from, from + QPointF(90, 120));
			from = boxPoint(item, {0, 0});
			gesture(from, from + QPointF(45, 60), Qt::AltModifier);
			click(pair, Qt::ControlModifier);
			from = boxPoint(item, {.5, .5});
			gesture(from, from + QPointF(20, 30));
			check(bothSelected(), "persisted final composition includes a completed multi-layer gesture");
			saved["pair_final"] = state(pair);
			saved["pair_id"] = double(obs_sceneitem_get_id(pair));
			saved["pair_uuid"] = obs_source_get_uuid(pairSource);
			check(equal(mainBefore, state(mainItem)),
			      "all direct edits leave the referenced Main scene item unchanged");
			check(obs_sceneitem_get_source(item) == obs_sceneitem_get_source(mainItem) &&
				      obs_source_get_width(obs_sceneitem_get_source(item)) == 360,
			      "gestures reuse the source and do not edit its global dimensions");
			OBSSourceAutoRelease program = obs_frontend_get_current_scene();
			check(program.Get() == mainSource.Get(),
			      "Undo and selection never replace the Main program with Portrait");
			saved["final"] = state(item);
			saved["portrait_uuid"] = obs_source_get_uuid(obs_scene_get_source(portrait()));
			saved["item_id"] = double(obs_sceneitem_get_id(item));
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
	write(restore ? "restore.json" : "runtime.json",
	      {{"passed", error.isEmpty()},
	       {"error", error},
	       {"checks", checks},
	       {"device_pixel_ratio", display ? display->devicePixelRatioF() : 0},
	       {"preview_width", display ? display->width() : 0},
	       {"preview_height", display ? display->height() : 0},
	       {"scope",
		"Native Linux Qt mouse events on the real portrait preview, libobs transforms/crop, native Undo/Redo and full restart persistence. Windows native execution pending."}});
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
	root = qEnvironmentVariable("OBS_STUDIO_PORTRAIT_GESTURES_ROOT");
	restore = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_GESTURES_RESTORE");
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
