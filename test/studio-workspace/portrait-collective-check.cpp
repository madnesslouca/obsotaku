/* Opt-in native Qt/libobs collective resize/crop regression.
 * All fixture sources are disposable. RESTORE checks a complete process restart.
 * Qt events are injected with an explicit activation/focus precondition; this is
 * not a physical pointer or compositor shortcut certification. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <graphics/matrix4.h>
#include <util/config-file.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QMainWindow>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScreen>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>

OBS_DECLARE_MODULE()
namespace {
constexpr const char *sceneName = "Portrait Collective QA";
constexpr const char *firstName = "Portrait Collective QA first";
constexpr const char *secondName = "Portrait Collective QA second";
constexpr const char *excludedName = "Portrait Collective QA excluded";
QString root;
QMainWindow *window = nullptr;
QWidget *display = nullptr;
QDockWidget *dock = nullptr;
QJsonObject saved;
QJsonArray checks;
bool restoring = false;
int optionsMaximumHeight = QWIDGETSIZE_MAX;
bool optionsAdjusted = false;
const std::array<QPointF, 8> handles = {QPointF(0, 0), QPointF(.5, 0), QPointF(1, 0), QPointF(1, .5),
					QPointF(1, 1), QPointF(.5, 1), QPointF(0, 1), QPointF(0, .5)};
template<class T> T *get(QObject *parent, const char *name)
{
	auto *value = parent->findChild<T *>(name);
	if (!value) {
		throw std::runtime_error(name);
	}
	return value;
}
void check(bool pass, const char *name, QJsonObject observed = {})
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}, {"observed", observed}});
	blog(LOG_INFO, "[portrait-collective-check] %s %s %s", pass ? "PASS" : "FAIL", name,
	     QJsonDocument(observed).toJson(QJsonDocument::Compact).constData());
	if (!pass) {
		throw std::runtime_error(name);
	}
}
void write(const char *name, QJsonObject object)
{
	QFile file(root + "/" + name);
	if (!file.open(QIODevice::WriteOnly)) {
		throw std::runtime_error("private report unavailable");
	}
	file.write(QJsonDocument(object).toJson());
}
bool wait(const std::function<bool()> &condition, int limit = 2200)
{
	for (int elapsed = 0; elapsed < limit; elapsed += 40) {
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
OBSSceneItem layer(const char *name)
{
	OBSScene current = portrait();
	return current ? obs_scene_find_source(current, name) : nullptr;
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
	      "portrait channel follows the requested Main UUID before testing");
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
		{"bw", i.bounds.x},
		{"bh", i.bounds.y},
		{"left", c.left},
		{"right", c.right},
		{"top", c.top},
		{"bottom", c.bottom}};
}
QJsonObject pairState()
{
	return {{"first", state(layer(firstName))}, {"second", state(layer(secondName))}};
}
bool equal(QJsonObject a, QJsonObject b, double epsilon = .04)
{
	if (a.keys() != b.keys()) {
		return false;
	}
	for (auto key : a.keys()) {
		if (a[key].isObject()) {
			if (!equal(a[key].toObject(), b[key].toObject(), epsilon)) {
				return false;
			}
		} else if (std::abs(a[key].toDouble() - b[key].toDouble()) > epsilon) {
			return false;
		}
	}
	return true;
}
QPointF map(const matrix4 &matrix, QPointF point)
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
QRectF selectionBox()
{
	double left = 1e20, top = 1e20, right = -1e20, bottom = -1e20;
	for (const char *name : {firstName, secondName}) {
		for (const QPointF &h : handles) {
			const QPointF p = boxPoint(layer(name), h);
			left = std::min(left, p.x());
			top = std::min(top, p.y());
			right = std::max(right, p.x());
			bottom = std::max(bottom, p.y());
		}
	}
	return {QPointF(left, top), QPointF(right, bottom)};
}
QPointF boxPoint(QRectF box, QPointF h)
{
	return box.topLeft() + QPointF(h.x() * box.width(), h.y() * box.height());
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
			return stable >= 18;
		},
		6500);
	check(allocated, "fixture waits for an exposed focused preview with stable geometry before press",
	      {{"display_width", display->width()},
	       {"display_height", display->height()},
	       {"window_width", top->width()},
	       {"window_height", top->height()},
	       {"focused", display->hasFocus()},
	       {"stable_turns", stable}});
}
void mouse(QEvent::Type type, QPointF canvas, Qt::KeyboardModifiers mods = {})
{
	// Readiness is a fixture precondition before a new gesture. Never wait
	// for it during mouse move/release or change a gesture's acceptance rules.
	if (type == QEvent::MouseButtonPress) {
		ready();
	}
	const QPointF p = pixel(canvas);
	QMouseEvent event(type, p, p, display->mapToGlobal(p),
			  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
			  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
	QApplication::sendEvent(display, &event);
	QTest::qWait(45);
}
void gesture(QPointF from, QPointF to, Qt::KeyboardModifiers mods = {})
{
	mouse(QEvent::MouseButtonPress, from, mods);
	for (int n = 1; n <= 5; ++n) {
		mouse(QEvent::MouseMove, from + (to - from) * n / 5., mods);
	}
	mouse(QEvent::MouseButtonRelease, to, mods);
	QTest::qWait(160);
}
void click(obs_sceneitem_t *item, Qt::KeyboardModifiers mods = {})
{
	const auto at = boxPoint(item, {.5, .5});
	gesture(at, at, mods);
}
void selectPair()
{
	get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
	gesture({1040, 1850}, {1040, 1850});
	click(layer(firstName));
	click(layer(secondName), Qt::ControlModifier);
	check(wait([] {
		      return obs_sceneitem_selected(layer(firstName)) && obs_sceneitem_selected(layer(secondName)) &&
			     get<QListView>(window, "sources")->selectionModel()->selectedRows().size() == 2;
	      }),
	      "Ctrl selection exposes two participants in the native source tree");
}
void reset(float firstRotation = 0, bool negative = false, bool bounds = false)
{
	get<QToolButton>(window, "studioPortraitCropMode")->setChecked(false);
	for (const char *name : {firstName, secondName}) {
		auto item = layer(name);
		const bool first = name == firstName;
		obs_transform_info i{};
		i.pos = first ? vec2{180, 450} : vec2{590, 1100};
		i.scale = {negative && first ? -1.f : 1.f, 1.f};
		if (negative && first) {
			i.pos.x += 360;
		}
		i.rot = first ? firstRotation : 0;
		i.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		i.bounds_type = bounds && !first ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_NONE;
		i.bounds = first ? vec2{360, 300} : vec2{240, 400};
		i.bounds_alignment = OBS_ALIGN_CENTER;
		obs_sceneitem_crop c{};
		obs_sceneitem_set_locked(item, false);
		obs_sceneitem_set_visible(item, true);
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_info2(item, &i);
		obs_sceneitem_set_crop(item, &c);
		obs_sceneitem_defer_update_end(item);
	}
	QTest::qWait(100);
	selectPair();
}
void undoRedo(QJsonObject before, QJsonObject after)
{
	check(!equal(before, after), "gesture changes the actual backend before testing history",
	      {{"before", before}, {"after", after}});
	get<QAction>(window, "actionMainUndo")->trigger();
	check(wait([&] { return equal(pairState(), before); }),
	      "one native Undo restores both full transforms and crop",
	      {{"actual", pairState()}, {"expected", before}});
	get<QAction>(window, "actionMainRedo")->trigger();
	check(wait([&] { return equal(pairState(), after); }), "one native Redo reapplies both participants",
	      {{"actual", pairState()}, {"expected", after}});
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
	for (const char *name : {sceneName, firstName, secondName, excludedName}) {
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
void fixture()
{
	OBSSourceAutoRelease collision = obs_get_source_by_name(sceneName);
	check(!collision, "fixture namespace is unused before creating temporary scenes");
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
	OBSSceneAutoRelease main = obs_scene_create(sceneName);
	for (const char *name : {firstName, secondName, excludedName}) {
		const bool first = name == firstName;
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_int(data, "width", first ? 360 : 240);
		obs_data_set_int(data, "height", first ? 300 : 400);
		obs_data_set_int(data, "color", first ? 0xff0099ee : 0xffff5511);
		OBSSourceAutoRelease source = obs_source_create("color_source_v3", name, data, nullptr);
		check(bool(source), "disposable native color source exists");
		auto *mainItem = obs_scene_add(main, source);
		const vec2 position{first ? 15.f : 45.f, first ? 25.f : 75.f};
		obs_sceneitem_set_pos(mainItem, &position);
	}
	go(obs_scene_get_source(main));
	for (const char *name : {firstName, secondName, excludedName}) {
		OBSSourceAutoRelease source = obs_get_source_by_name(name);
		obs_scene_add(portrait(), source);
	}
	obs_scene_enum_items(
		portrait(),
		[](obs_scene_t *, obs_sceneitem_t *item, void *) {
			OBSDataAutoRelease privateData = obs_sceneitem_get_private_settings(item);
			if (obs_data_get_bool(privateData, "otaku_mirror")) {
				obs_sceneitem_set_visible(item, false);
			}
			return true;
		},
		nullptr);
	auto excluded = layer(excludedName);
	vec2 p{850, 100};
	obs_sceneitem_set_pos(excluded, &p);
	obs_sceneitem_set_locked(excluded, true);
}
void run()
{
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active(), "collective fixture starts with all encoders idle");
		if (restoring) {
			QFile file(root + "/saved.json");
			check(file.open(QIODevice::ReadOnly),
			      "restart baseline exists in the private fixture directory");
			saved = QJsonDocument::fromJson(file.readAll()).object();
			mode(true);
			OBSSourceAutoRelease main = obs_get_source_by_name(sceneName);
			check(bool(main), "restart restores the disposable Main scene");
			go(main);
		} else {
			fixture();
		}
		dock = get<QDockWidget>(window, "verticalPreviewDock");
		dock->setFloating(true);
		dock->show();
		const QRect available = dock->screen()->availableGeometry();
		dock->resize(std::min(520, available.width() - 60), std::min(1000, available.height() - 60));
		dock->raise();
		display = get<QWidget>(window, "verticalPreviewDisplay");
		auto *options = get<QScrollArea>(window, "verticalPreviewOptionsScroll");
		optionsMaximumHeight = options->maximumHeight();
		options->setMaximumHeight(140);
		optionsAdjusted = true;
		check(wait([] { return display->isVisible() && display->width() >= 180 && display->height() >= 320; }),
		      "native preview is visible with usable geometry");
		ready();
		auto *cropButton = get<QToolButton>(window, "studioPortraitCropMode");
		const QRect displayRect(display->mapTo(dock, QPoint()), display->size());
		const QRect cropRect(cropButton->mapTo(dock, QPoint()), cropButton->size());
		check(cropButton->isVisible() && dock->rect().contains(cropRect) && !displayRect.intersects(cropRect),
		      "Crop button is visible inside dock and outside preview at this DPI");
		check(layer(firstName) && layer(secondName) && layer(excludedName), "all saved fixture items exist");
		OBSSourceAutoRelease main = obs_get_source_by_name(sceneName);
		auto *mainFirst = obs_scene_find_source(obs_scene_from_source(main), firstName);
		auto *mainSecond = obs_scene_find_source(obs_scene_from_source(main), secondName);
		const QJsonObject mainBefore{{"first", state(mainFirst)}, {"second", state(mainSecond)}};
		if (restoring) {
			check(saved["portrait_uuid"].toString() ==
				      obs_source_get_uuid(obs_scene_get_source(portrait())),
			      "restart retains the linked portrait scene UUID");
			check(equal(pairState(), saved["final"].toObject()),
			      "both participants retain collective resize and crop after a complete restart",
			      {{"expected", saved["final"]}, {"actual", pairState()}});
			for (const char *name : {firstName, secondName}) {
				const QString key = name == firstName ? "first" : "second";
				auto item = layer(name);
				const auto identity = saved[key + "_identity"].toObject();
				check(identity["item"].toVariant().toLongLong() == obs_sceneitem_get_id(item) &&
					      identity["source"].toString() ==
						      obs_source_get_uuid(obs_sceneitem_get_source(item)),
				      "restart retains participant item ID and shared source UUID");
			}
			check(equal(mainBefore, saved["main_before"].toObject()),
			      "shared Main transforms and crop survive collective persistence unchanged");
			check(equal(state(layer(excludedName)), saved["excluded_final"].toObject()),
			      "excluded locked source retains its independent transform after restart");
			selectPair();
			check(crop(layer(firstName)).left > 0 && crop(layer(secondName)).left > 0 &&
				      std::abs(info(layer(firstName)).scale.x) > 1,
			      "saved final composition contains actual resize and nonzero crop");
			cleanup();
		} else {
			const auto excludedBefore = state(layer(excludedName));
			// Eight AABB handles: default uniform factor and opposite anchor.
			for (const QPointF h : handles) {
				reset();
				const auto before = pairState();
				const QRectF box = selectionBox();
				const QPointF from = boxPoint(box, h);
				const QPointF anchor = boxPoint(box, {1 - h.x(), 1 - h.y()});
				const QPointF delta((h.x() - .5) * box.width() * .24,
						    (h.y() - .5) * box.height() * .24);
				const QPointF firstOrigin = boxPoint(layer(firstName), {0, 0});
				const QPointF secondOrigin = boxPoint(layer(secondName), {0, 0});
				gesture(from, from + delta);
				const double expected = 1.12;
				auto a = info(layer(firstName)), b = info(layer(secondName));
				check(std::abs(a.scale.x - expected) < .006 && std::abs(a.scale.y - expected) < .006 &&
					      std::abs(b.scale.x - expected) < .006 &&
					      std::abs(b.scale.y - expected) < .006,
				      "each collective corner or side scales both participants uniformly",
				      {{"handle_x", h.x()}, {"handle_y", h.y()}, {"actual", pairState()}});
				check(QLineF(boxPoint(layer(firstName), {0, 0}),
					     anchor + (firstOrigin - anchor) * expected)
							      .length() < 1.1 &&
					      QLineF(boxPoint(layer(secondName), {0, 0}),
						     anchor + (secondOrigin - anchor) * expected)
							      .length() < 1.1,
				      "collective resize transforms both origins around the opposite global anchor");
				undoRedo(before, pairState());
			}
			// Free scaling maps global x/y independently only without shear.
			reset();
			auto before = pairState();
			auto box = selectionBox();
			gesture(box.bottomRight(), box.bottomRight() + QPointF(box.width() * .1, box.height() * .1),
				Qt::ControlModifier);
			check(obs_sceneitem_selected(layer(firstName)) && obs_sceneitem_selected(layer(secondName)) &&
				      std::abs(info(layer(firstName)).scale.x - 1.1) < .006 &&
				      std::abs(info(layer(secondName)).scale.x - 1.1) < .006,
			      "holding Ctrl while taking a collective handle preserves selection and resizes the set");
			undoRedo(before, pairState());
			reset();
			before = pairState();
			box = selectionBox();
			gesture(box.bottomRight(), box.bottomRight() + QPointF(box.width() * .18, box.height() * .08),
				Qt::ShiftModifier);
			check(std::abs(info(layer(firstName)).scale.x - 1.18) < .006 &&
				      std::abs(info(layer(secondName)).scale.x - 1.18) < .006 &&
				      std::abs(info(layer(firstName)).scale.y - 1.08) < .006 &&
				      std::abs(info(layer(secondName)).scale.y - 1.08) < .006,
			      "Shift permits representable nonuniform collective resize on axis-aligned items",
			      {{"actual", pairState()}});
			undoRedo(before, pairState());

			reset(90);
			before = pairState();
			box = selectionBox();
			gesture(box.bottomRight(), box.bottomRight() + QPointF(box.width() * .16, box.height() * .08),
				Qt::ShiftModifier);
			check(std::abs(info(layer(firstName)).scale.x - 1.08) < .006 &&
				      std::abs(info(layer(firstName)).scale.y - 1.16) < .006 &&
				      std::abs(info(layer(secondName)).scale.x - 1.16) < .006 &&
				      std::abs(info(layer(secondName)).scale.y - 1.08) < .006,
			      "quarter-turn participants map free global scaling to the correct local axes",
			      {{"actual", pairState()}});
			undoRedo(before, pairState());

			reset(27);
			const auto sentinelBefore = pairState();
			const auto sentinelCenter = boxPoint(layer(secondName), {.5, .5});
			gesture(sentinelCenter, sentinelCenter + QPointF(15, 20));
			before = pairState();
			check(!equal(sentinelBefore, before),
			      "history sentinel is a real collective move before rejection");
			box = selectionBox();
			const auto actionBefore = get<QAction>(window, "actionMainUndo")->text();
			const QPointF end = box.bottomRight() + QPointF(box.width() * .17, box.height() * .07);
			gesture(box.bottomRight(), end, Qt::ShiftModifier);
			check(equal(before, pairState()) &&
				      get<QAction>(window, "actionMainUndo")->text() == actionBefore,
			      "unsupported shearing resize rejects the whole set without history",
			      {{"actual", pairState()}});
			get<QAction>(window, "actionMainUndo")->trigger();
			check(wait([&] { return equal(pairState(), sentinelBefore); }),
			      "one Undo after a rejected resize reaches the previous real action");
			get<QAction>(window, "actionMainRedo")->trigger();
			check(wait([&] { return equal(pairState(), before); }),
			      "Redo after rejection restores only the history sentinel");
			// Rejecting one movement must not poison the following valid gesture.
			mouse(QEvent::MouseButtonPress, box.bottomRight(), Qt::ShiftModifier);
			mouse(QEvent::MouseMove, end, Qt::ShiftModifier);
			check(equal(before, pairState()) &&
				      display->property("studioPortraitGestureStatus").toString() ==
					      "unsupported_shear",
			      "invalid Shift movement leaves every participant untouched and exposes explicit rejection");
			mouse(QEvent::MouseMove, end);
			mouse(QEvent::MouseButtonRelease, end);
			check(std::abs(info(layer(firstName)).scale.x - 1.17) < .006 &&
				      std::abs(info(layer(firstName)).scale.y - 1.17) < .006 &&
				      std::abs(info(layer(firstName)).rot - 27) < .01,
			      "releasing Shift recovers uniform resize in the same gesture without altering rotation");
			undoRedo(before, pairState());

			reset(23, true, true);
			before = pairState();
			box = selectionBox();
			gesture(box.bottomRight(), box.bottomRight() + QPointF(box.width() * .1, box.height() * .1));
			check(std::abs(info(layer(firstName)).scale.x + 1.1) < .006 &&
				      std::abs(info(layer(firstName)).scale.y - 1.1) < .006 &&
				      std::abs(info(layer(secondName)).bounds.x - 264) < .8 &&
				      std::abs(info(layer(secondName)).bounds.y - 440) < .8 &&
				      std::abs(info(layer(secondName)).scale.x - 1) < .001,
			      "uniform resize preserves negative scale and rotation while scaling bounds separately",
			      {{"actual", pairState()}});
			undoRedo(before, pairState());

			// Crop fractions refer to each item's visible local pixels, not to
			// the shared source properties or its rotation in canvas space.
			for (int handleIndex : {0, 2, 4, 6, 1, 3, 5, 7}) {
				reset();
				before = pairState();
				box = selectionBox();
				const QPointF h = handles[handleIndex];
				const QPointF from = boxPoint(box, h);
				const QPointF delta(h.x() == .5  ? 0
						    : h.x() == 0 ? box.width() * .1
								 : -box.width() * .1,
						    h.y() == .5  ? 0
						    : h.y() == 0 ? box.height() * .1
								 : -box.height() * .1);
				const QPointF firstAnchor = boxPoint(layer(firstName), {1 - h.x(), 1 - h.y()});
				const QPointF secondAnchor = boxPoint(layer(secondName), {1 - h.x(), 1 - h.y()});
				gesture(from, from + delta, Qt::AltModifier);
				const auto ca = crop(layer(firstName)), cb = crop(layer(secondName));
				check(ca.left == (h.x() == 0 ? 36 : 0) && cb.left == (h.x() == 0 ? 24 : 0) &&
					      ca.right == (h.x() == 1 ? 36 : 0) && cb.right == (h.x() == 1 ? 24 : 0) &&
					      ca.top == (h.y() == 0 ? 30 : 0) && cb.top == (h.y() == 0 ? 40 : 0) &&
					      ca.bottom == (h.y() == 1 ? 30 : 0) && cb.bottom == (h.y() == 1 ? 40 : 0),
				      "each collective crop handle applies equal local fractions to different source sizes",
				      {{"handle", handleIndex}, {"actual", pairState()}});
				check(QLineF(firstAnchor, boxPoint(layer(firstName), {1 - h.x(), 1 - h.y()})).length() <
						      1 &&
					      QLineF(secondAnchor, boxPoint(layer(secondName), {1 - h.x(), 1 - h.y()}))
							      .length() < 1,
				      "collective unbounded crop keeps each opposite local edge or corner fixed");
				undoRedo(before, pairState());
			}
			reset(21, true, true);
			before = pairState();
			box = selectionBox();
			cropButton->setChecked(true);
			QTest::qWait(60);
			gesture(box.topLeft(), box.topLeft() + QPointF(box.width() * .125, box.height() * .1));
			const auto boundAfter = info(layer(secondName));
			check(crop(layer(firstName)).left == 45 && crop(layer(secondName)).left == 30 &&
				      crop(layer(firstName)).top == 30 && crop(layer(secondName)).top == 40,
			      "Crop toggle applies local fractions to mirrored rotated and bounded participants",
			      {{"actual", pairState()}});
			check(boundAfter.pos.x == before["second"].toObject()["x"].toDouble() &&
				      boundAfter.pos.y == before["second"].toObject()["y"].toDouble() &&
				      boundAfter.bounds.x == 240 && boundAfter.bounds.y == 400,
			      "bounded crop keeps bounds and placement while OBS refits visible content");
			undoRedo(before, pairState());
			cropButton->setChecked(false);

			reset();
			before = pairState();
			box = selectionBox();
			gesture(box.topLeft(), box.topLeft() + QPointF(box.width() * 1.3, box.height() * 1.3),
				Qt::AltModifier);
			const auto clampA = crop(layer(firstName)), clampB = crop(layer(secondName));
			const int aw = 360 - clampA.left - clampA.right, ah = 300 - clampA.top - clampA.bottom;
			const int bw = 240 - clampB.left - clampB.right, bh = 400 - clampB.top - clampB.bottom;
			check(aw >= 2 && ah >= 2 && bw >= 2 && bh >= 2 && std::min(aw, bw) == 2 &&
				      std::min(ah, bh) == 2,
			      "collective inward crop clamps every participant to at least two visible source pixels",
			      {{"actual", pairState()}});
			check(std::abs(double(clampA.left) / 360 - double(clampB.left) / 240) < .005 &&
				      std::abs(double(clampA.top) / 300 - double(clampB.top) / 400) < .005,
			      "clamp is shared across the set rather than clipping each source at different fractions");
			undoRedo(before, pairState());

			reset();
			obs_sceneitem_crop ca{}, cb{};
			ca.left = 40;
			ca.top = 30;
			cb.left = 20;
			cb.top = 80;
			obs_sceneitem_set_crop(layer(firstName), &ca);
			obs_sceneitem_set_crop(layer(secondName), &cb);
			QTest::qWait(100);
			before = pairState();
			box = selectionBox();
			gesture(box.topLeft(), box.topLeft() - QPointF(box.width() * .3, box.height() * .3),
				Qt::AltModifier);
			const auto outA = crop(layer(firstName)), outB = crop(layer(secondName));
			check(outA.left >= 0 && outB.left == 0 && outA.top == 0 && outB.top >= 0,
			      "outward crop restores existing crop using the most restrictive shared fraction",
			      {{"actual", pairState()}});
			undoRedo(before, pairState());

			// Cancellation rolls the complete set back without an Undo action.
			for (int reason = 0; reason < 5; ++reason) {
				reset();
				before = pairState();
				box = selectionBox();
				const auto undoLabel = get<QAction>(window, "actionMainUndo")->text();
				mouse(QEvent::MouseButtonPress, box.bottomRight(),
				      reason == 4 ? Qt::AltModifier : Qt::NoModifier);
				const auto to = box.bottomRight() + (reason == 4 ? QPointF(-70, -90) : QPointF(70, 90));
				mouse(QEvent::MouseMove, to, reason == 4 ? Qt::AltModifier : Qt::NoModifier);
				check(!equal(before, pairState()),
				      "incomplete collective gesture actually moved before cancellation");
				if (reason == 0 || reason == 4) {
					QTest::keyClick(display, Qt::Key_Escape);
				} else if (reason == 1) {
					obs_sceneitem_set_locked(layer(secondName), true);
				} else if (reason == 2) {
					obs_sceneitem_set_visible(layer(secondName), false);
				} else {
					get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(0);
				}
				QTest::qWait(140);
				mouse(QEvent::MouseButtonRelease, to);
				check(equal(before, pairState()) &&
					      get<QAction>(window, "actionMainUndo")->text() == undoLabel,
				      "Escape lock hide or context change cancels all transforms and crop without history",
				      {{"reason", reason}, {"actual", pairState()}});
			}
			reset();
			before = pairState();
			box = selectionBox();
			mouse(QEvent::MouseButtonPress, box.bottomRight());
			mouse(QEvent::MouseMove, box.bottomRight() + QPointF(60, 75));
			check(!equal(before, pairState()),
			      "removal cancellation begins from an actual unfinished resize");
			OBSSceneItem removed = layer(secondName);
			obs_sceneitem_remove(removed);
			QTest::qWait(140);
			mouse(QEvent::MouseButtonRelease, box.bottomRight() + QPointF(60, 75));
			check(equal(before["first"].toObject(), state(layer(firstName))) && !layer(secondName) &&
				      !obs_sceneitem_get_scene(removed),
			      "removing one participant restores survivors without resurrecting the removed item");
			OBSSourceAutoRelease second = obs_get_source_by_name(secondName);
			obs_scene_add(portrait(), second);
			reset();

			// Excluded locked items must not enter the gesture or its snapshot.
			auto excluded = layer(excludedName);
			obs_sceneitem_select(excluded, true);
			before = pairState();
			box = selectionBox();
			gesture(box.bottomRight(), box.bottomRight() + QPointF(box.width() * .1, box.height() * .1));
			auto after = pairState();
			check(equal(excludedBefore, state(excluded)),
			      "locked selected item is excluded from collective resize geometry");
			vec2 externalPosition{890, 140};
			obs_sceneitem_set_pos(excluded, &externalPosition);
			const auto excludedExternal = state(excluded);
			undoRedo(before, after);
			check(equal(excludedExternal, state(excluded)),
			      "collective Undo and Redo preserve later changes to an excluded locked item");
			check(equal(mainBefore,
				    QJsonObject{{"first", state(mainFirst)}, {"second", state(mainSecond)}}),
			      "collective gestures never modify either shared Main item or its crop");
			check(obs_sceneitem_get_source(layer(firstName)) == obs_sceneitem_get_source(mainFirst) &&
				      obs_source_get_width(obs_sceneitem_get_source(layer(firstName))) == 360 &&
				      obs_source_get_height(obs_sceneitem_get_source(layer(secondName))) == 400,
			      "shared source identity and global dimensions are unchanged");

			// Persist a demonstrably resized and cropped composition for restart.
			reset();
			box = selectionBox();
			gesture(box.bottomRight(), box.bottomRight() + QPointF(box.width() * .15, box.height() * .15));
			box = selectionBox();
			gesture(box.topLeft(), box.topLeft() + QPointF(box.width() * .1, box.height() * .1),
				Qt::AltModifier);
			saved["final"] = pairState();
			saved["main_before"] = mainBefore;
			saved["excluded_final"] = state(layer(excludedName));
			saved["portrait_uuid"] = obs_source_get_uuid(obs_scene_get_source(portrait()));
			for (const char *name : {firstName, secondName}) {
				const QString key = name == firstName ? "first" : "second";
				auto item = layer(name);
				saved[key + "_identity"] =
					QJsonObject{{"item", double(obs_sceneitem_get_id(item))},
						    {"source", obs_source_get_uuid(obs_sceneitem_get_source(item))}};
			}
			write("saved.json", saved);
			obs_frontend_save();
			check(crop(layer(firstName)).left > 0 && crop(layer(secondName)).left > 0 &&
				      info(layer(firstName)).scale.x > 1.1 && info(layer(secondName)).scale.x > 1.1,
			      "restart fixture saves real collective resize and crop on both participants");
			display->grab().save(root + "/preview.png");
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
	       {"device_pixel_ratio", display ? display->devicePixelRatioF() : 0},
	       {"preview_width", display ? display->width() : 0},
	       {"preview_height", display ? display->height() : 0},
	       {"scope",
		"Injected native Qt pointer events, libobs collective transforms/crop, native Undo/Redo and full process restart. Synthetic sources; no physical-device, Windows-native, output-concurrency or compositor shortcut certification."}});
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
	root = qEnvironmentVariable("OBS_STUDIO_PORTRAIT_COLLECTIVE_ROOT");
	restoring = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_COLLECTIVE_RESTORE");
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
