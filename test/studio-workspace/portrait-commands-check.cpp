/* Opt-in native dock command regression. Uses disposable scene/source fixtures.
 * RESTORE=1 verifies saved graph/IDs/settings after a full process restart. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QScrollArea>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <atomic>
#include <functional>
#include <stdexcept>
OBS_DECLARE_MODULE()
namespace {
QString root;
QMainWindow *window;
QJsonObject saved;
QJsonArray checks;
bool restore;
constexpr const char *fixtureName = "Portrait Commands QA";
constexpr const char *sharedName = "Portrait Commands QA shared";
constexpr const char *otherName = "Portrait Commands QA other";
constexpr const char *groupName = "Portrait Commands QA group";
constexpr const char *onlyName = "Portrait Commands QA only";
constexpr const char *fitKey = "studioPortraitMirrorFit";
std::atomic<int> rootFilterLoads{0};
struct LoadProbe {
	obs_source_t *source;
};
obs_source_info loadProbeInfo{};
void registerLoadProbe()
{
	loadProbeInfo.id = "portrait_commands_load_probe";
	loadProbeInfo.type = OBS_SOURCE_TYPE_FILTER;
	loadProbeInfo.output_flags = OBS_SOURCE_VIDEO;
	loadProbeInfo.get_name = [](void *) {
		return "QA root filter load counter";
	};
	loadProbeInfo.create = [](obs_data_t *, obs_source_t *source) -> void * {
		return new LoadProbe{source};
	};
	loadProbeInfo.destroy = [](void *data) {
		delete static_cast<LoadProbe *>(data);
	};
	loadProbeInfo.load = [](void *, obs_data_t *) {
		++rootFilterLoads;
	};
	loadProbeInfo.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<LoadProbe *>(data)->source);
	};
	obs_register_source(&loadProbeInfo);
}
template<class T> T *get(QObject *parent, const char *name)
{
	auto *p = parent->findChild<T *>(name);
	if (!p) {
		throw std::runtime_error(name);
	}
	return p;
}
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}});
	blog(LOG_INFO, "[portrait-commands-check] %s %s", pass ? "PASS" : "FAIL", name);
	if (!pass) {
		throw std::runtime_error(name);
	}
}
void write(const char *name, QJsonObject data)
{
	QFile file(root + "/" + name);
	if (!file.open(QIODevice::WriteOnly)) {
		throw std::runtime_error("report unavailable");
	}
	file.write(QJsonDocument(data).toJson());
}
void mode(bool multi)
{
	bool ok = false;
	check(QMetaObject::invokeMethod(window, "SetStudioCanvasMode", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
					Q_ARG(bool, multi)) &&
		      ok,
	      "mode action succeeds");
	QTest::qWait(150);
}
OBSScene portrait()
{
	OBSCanvasAutoRelease canvas = obs_get_canvas_by_name("Vertical 9:16");
	OBSSourceAutoRelease source = canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
	return source ? obs_scene_from_source(source) : nullptr;
}
void go(obs_source_t *source)
{
	obs_frontend_set_current_scene(source);
	bool linked = false;
	for (int attempt = 0; attempt < 80; ++attempt) {
		OBSScene current = portrait();
		OBSDataAutoRelease priv = current ? obs_source_get_private_settings(obs_scene_get_source(current))
						  : nullptr;
		if (current && QString::fromUtf8(obs_data_get_string(priv, "otaku_main_scene_uuid")) ==
				       obs_source_get_uuid(source)) {
			linked = true;
			break;
		}
		QTest::qWait(50);
	}
	check(linked, "scene request resolves to its own linked portrait composition");
	QTest::qWait(150);
}

OBSSceneItem item(const char *name = sharedName)
{
	return obs_scene_find_source(portrait(), name);
}
OBSSceneItem mirror()
{
	OBSSceneItem found;
	obs_scene_enum_items(
		portrait(),
		[](obs_scene_t *, obs_sceneitem_t *i, void *data) {
			OBSDataAutoRelease p = obs_sceneitem_get_private_settings(i);
			if (obs_data_get_bool(p, "otaku_mirror")) {
				*static_cast<OBSSceneItem *>(data) = i;
				return false;
			}
			return true;
		},
		&found);
	return found;
}
obs_transform_info info(obs_sceneitem_t *i)
{
	obs_transform_info v{};
	obs_sceneitem_get_info2(i, &v);
	return v;
}
QJsonValue rounded(QJsonValue value)
{
	if (value.isObject()) {
		auto o = value.toObject();
		for (auto k : o.keys()) {
			o[k] = rounded(o[k]);
		}
		return o;
	}
	if (value.isArray()) {
		QJsonArray a;
		for (auto v : value.toArray()) {
			a.append(rounded(v));
		}
		return a;
	}
	if (value.isDouble()) {
		return std::round(value.toDouble() * 100000.) / 100000.;
	}
	return value;
}
QJsonObject state(obs_scene_t *scene)
{
	OBSDataAutoRelease d = obs_save_source(obs_scene_get_source(scene));
	OBSDataAutoRelease settings = obs_data_get_obj(d, "settings");
	OBSDataAutoRelease p = obs_source_get_private_settings(obs_scene_get_source(scene));
	return {{"settings", rounded(QJsonDocument::fromJson(obs_data_get_json(settings)).object())},
		{"has_fit", obs_data_has_user_value(p, fitKey)},
		{"fit", int(obs_data_get_int(p, fitKey))}};
}
void select(const char *name)
{
	get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(1);
	auto *tree = get<QListView>(window, "sources");
	for (int attempt = 0; attempt < 40; ++attempt) {
		for (int row = 0; row < tree->model()->rowCount(); ++row) {
			auto index = tree->model()->index(row, 0);
			if (index.data(Qt::AccessibleTextRole).toString() == name) {
				tree->scrollTo(index);
				QTest::mouseClick(tree->viewport(), Qt::LeftButton, {},
						  tree->visualRect(index).center());
				QTest::qWait(220);
				return;
			}
		}
		QTest::qWait(50);
	}
	QStringList names;
	for (int row = 0; row < tree->model()->rowCount(); ++row) {
		names << tree->model()->index(row, 0).data(Qt::AccessibleTextRole).toString();
	}
	blog(LOG_ERROR, "[portrait-commands-check] wanted %s; context=%s; native rows=%s; root=%s", name,
	     window->property("studioEditingCanvas").toString().toUtf8().constData(),
	     names.join(" | ").toUtf8().constData(), obs_source_get_name(obs_scene_get_source(portrait())));
	throw std::runtime_error("selection fixture missing from native tree");
}

QToolButton *button(const char *command)
{
	for (auto *b : window->findChildren<QToolButton *>()) {
		if (b->property("studioCommand").toString() == command) {
			return b;
		}
	}
	throw std::runtime_error(command);
}
void click(QToolButton *b)
{
	for (QWidget *p = b->parentWidget(); p; p = p->parentWidget()) {
		if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
			scroll->ensureWidgetVisible(b);
		}
	}
	QTest::qWait(60);
	check(b->isVisible() && b->isEnabled(), "requested command is visible and enabled");
	QTest::mouseClick(b, Qt::LeftButton);
	QTest::qWait(350);
}
void undo()
{
	get<QAction>(window, "actionMainUndo")->trigger();
	QTest::qWait(420);
}
void redo()
{
	get<QAction>(window, "actionMainRedo")->trigger();
	QTest::qWait(420);
}
void cycle(const QJsonObject &before, const QJsonObject &after)
{
	undo();
	check(state(portrait()) == before,
	      "one Undo restores layer graph order transforms visibility and framing state");
	redo();
	check(state(portrait()) == after, "one Redo restores the complete dock command");
}
int count(obs_scene_t *scene)
{
	int n = 0;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *, void *p) {
			++*static_cast<int *>(p);
			return true;
		},
		&n);
	return n;
}
QListWidgetItem *aux(const char *name)
{
	auto *list = get<QListWidget>(window, "verticalPreviewLayerList");
	for (int n = 0; n < list->count(); ++n) {
		if (list->item(n)->text() == name) {
			return list->item(n);
		}
	}
	throw std::runtime_error("auxiliary layer missing");
}
void addMenu(const QString &uuid, bool changeScene = false)
{
	QString error;
	QTimer::singleShot(100, qApp, [&]() {
		auto *menu = window->findChild<QMenu *>("studioPortraitAddMenu");
		try {
			check(menu && menu->isVisible(),
			      "portrait Add constructs and shows the reference menu");
			if (changeScene) {
				OBSSourceAutoRelease original =
					obs_get_source_by_uuid(saved["scene"].toString().toUtf8().constData());
				obs_frontend_set_current_scene(original);
				QTest::qWait(500);
			}
			QAction *target = nullptr;
			for (auto *action : menu->actions()) {
				if ((uuid == "whole" && action->objectName() == "studioPortraitAddWholeScene") ||
				    action->data().toString() == uuid) {
					target = action;
				}
			}
			check(target != nullptr, "requested existing source is offered by reference");
			QTest::mouseClick(menu, Qt::LeftButton, {}, menu->actionGeometry(target).center());
		} catch (const std::exception &e) {
			error = e.what();
		}
		if (menu) {
			menu->close();
		}
	});
	click(button("add"));
	if (!error.isEmpty()) {
		throw std::runtime_error(error.toStdString());
	}
}
void framing(bool fit)
{
	auto *combo = get<QComboBox>(window, "verticalPreviewFraming");
	for (QWidget *p = combo->parentWidget(); p; p = p->parentWidget()) {
		if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
			scroll->ensureWidgetVisible(combo);
		}
	}
	combo->setFocus();
	QTest::keyClick(combo, fit ? Qt::Key_End : Qt::Key_Home);
	QTest::qWait(350);
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
	for (const char *name : {fixtureName, "Portrait Commands QA second", sharedName, otherName, onlyName}) {
		OBSSourceAutoRelease s = obs_get_source_by_name(name);
		if (s) {
			obs_source_remove(s);
		}
	}
	mode(saved["multi"].toBool());
	if (!saved["had_multi"].toBool()) {
		config_remove_value(obs_frontend_get_user_config(), "BasicWindow", "StudioMultiCanvas");
	}
	config_save_safe(obs_frontend_get_user_config(), "tmp", nullptr);
	if (saved["studio"].toBool()) {
		obs_frontend_set_preview_program_mode(true);
		OBSSourceAutoRelease p = obs_get_source_by_uuid(saved["preview"].toString().toUtf8().constData());
		if (p) {
			obs_frontend_set_current_preview_scene(p);
		}
	}
	QTest::qWait(200);
	window->restoreState(QByteArray::fromBase64(saved["layout"].toString().toUtf8()));
	obs_frontend_save();
}
void run()
{
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active(), "dock command fixture begins with outputs idle");
		if (restore) {
			QFile f(root + "/saved.json");
			check(f.open(QIODevice::ReadOnly), "private restart baseline exists");
			saved = QJsonDocument::fromJson(f.readAll()).object();
		} else {
			// Recover only this probe's fixtures from an interrupted prior attempt.
			QFile oldBaseline(root + "/saved.json");
			OBSSourceAutoRelease stale = obs_get_source_by_name(fixtureName);
			if (stale && oldBaseline.open(QIODevice::ReadOnly)) {
				saved = QJsonDocument::fromJson(oldBaseline.readAll()).object();
				cleanup();
				stale = nullptr;
				saved = {};
				checks = {};
				QTimer::singleShot(700, qApp, run);
				return;
			}
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
			OBSSceneAutoRelease fixture = obs_scene_create(fixtureName);
			OBSDataAutoRelease d = obs_data_create();
			obs_data_set_int(d, "width", 360);
			obs_data_set_int(d, "height", 240);
			obs_data_set_int(d, "color", 0xff3388ff);
			OBSSourceAutoRelease shared = obs_source_create("color_source_v3", sharedName, d, nullptr),
					     other = obs_source_create("color_source_v3", otherName, d, nullptr);
			auto *main = obs_scene_add(fixture, shared);
			vec2 pos{70, 90};
			obs_sceneitem_set_pos(main, &pos);
			obs_scene_add(fixture, other);
			go(obs_scene_get_source(fixture));
			check(bool(portrait()), "fixture owns an independent linked portrait scene");
			auto *copy = obs_scene_add(portrait(), shared);
			obs_transform_info i{};
			i.pos = {180, 280};
			i.scale = {.75f, -.9f};
			i.rot = 13;
			i.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
			obs_sceneitem_set_info2(copy, &i);
			obs_sceneitem_crop c{1, 2, 3, 4};
			obs_sceneitem_set_crop(copy, &c);
			obs_sceneitem_set_scale_filter(copy, OBS_SCALE_POINT);
			obs_scene_add(portrait(), other);
			OBSSourceAutoRelease rootFilter = obs_source_create(
				"portrait_commands_load_probe", "Portrait Commands QA root filter", nullptr, nullptr);
			check(bool(rootFilter), "native root filter load counter is available");
			rootFilterLoads.store(0);
			obs_source_filter_add(obs_scene_get_source(portrait()), rootFilter);
		}
		mode(true);
		auto *dock = get<QDockWidget>(window, "verticalPreviewDock");
		dock->setFloating(true);
		dock->resize(540, 1200);
		dock->show();
		dock->raise();
		QTest::qWait(300);
		OBSSourceAutoRelease mainSource = obs_get_source_by_name(fixtureName);
		check(bool(mainSource), "fixture Main scene remains available");
		auto *mainScene = obs_scene_from_source(mainSource);
		auto *mainItem = obs_scene_find_source(mainScene, sharedName);
		const auto mainInitial = state(mainScene);
		if (restore) {
			check(saved["portrait_uuid"].toString() ==
				      obs_source_get_uuid(obs_scene_get_source(portrait())),
			      "restart retains the same portrait scene UUID");
			check(state(portrait()) == saved["final"].toObject(),
			      "dock order visibility transforms and mirror preference persist after full restart");
			check(bool(item(groupName)), "restored removed group remains present after restart");
			OBSSourceAutoRelease child =
				obs_get_source_by_uuid(saved["child_uuid"].toString().toUtf8().constData());
			check(child && obs_source_filter_count(child) == 1,
			      "recovered group child and effect persist after restart");
			check(info(mainItem).pos.x == 70 && info(mainItem).pos.y == 90,
			      "Main item remains unchanged after command persistence");
			cleanup();
		} else {
			select(sharedName);
			auto before = state(portrait());
			click(button("up"));
			auto after = state(portrait());
			check(before != after, "raise button immediately changes the portrait order");
			cycle(before, after);
			select(sharedName);
			before = state(portrait());
			click(button("down"));
			after = state(portrait());
			check(before != after, "lower button immediately changes the portrait order");
			cycle(before, after);
			select(sharedName);
			before = state(portrait());
			click(button("fit"));
			after = state(portrait());
			check(info(item()).bounds_type == OBS_BOUNDS_SCALE_INNER && info(item()).pos.x == 540 &&
				      info(item()).pos.y == 960,
			      "Fit updates bounds and centers the portrait item");
			cycle(before, after);
			undo();
			check(obs_sceneitem_get_scale_filter(item()) == OBS_SCALE_POINT,
			      "Undo Fit also restores the sampling filter omitted by transform-only snapshots");
			redo();
			select(sharedName);
			QString history = get<QAction>(window, "actionMainUndo")->text();
			click(button("fit"));
			check(get<QAction>(window, "actionMainUndo")->text() == history,
			      "repeated identical Fit does not add a history entry");
			undo();
			check(state(portrait()) == before,
			      "Undo after repeated Fit reaches the pre-fit state without a duplicate action");
			redo();
			// A source shared with Main must retain its current global settings and item identity.
			select(sharedName);
			before = state(portrait());
			get<QComboBox>(window, "studioActiveCanvas")->setCurrentIndex(0);
			click(button("remove"));
			after = state(portrait());
			check(!item() && count(portrait()) == 2, "Remove deletes only the selected portrait reference");
			vec2 moved{77, 99};
			obs_sceneitem_set_pos(mainItem, &moved);
			OBSSource sharedSource = obs_sceneitem_get_source(mainItem);
			OBSDataAutoRelease changed = obs_data_create();
			obs_data_set_int(changed, "width", 500);
			obs_source_update(sharedSource, changed);
			undo();
			check(bool(item()) && obs_sceneitem_get_source(item()) == sharedSource.Get(),
			      "Undo removal reuses the existing shared source instance");
			check(obs_scene_find_source(mainScene, sharedName) == mainItem && info(mainItem).pos.x == 77 &&
				      obs_source_get_width(sharedSource) == 500,
			      "Undo does not reload Main or overwrite newer shared input settings");
			check(window->property("studioEditingCanvas").toString() == "portrait" &&
				      get<QLabel>(window, "studioSelectedSourceName")->text() == sharedName,
			      "Undo navigates to Portrait and restores native inspector selection");
			redo();
			check(!item(), "Redo removes the same portrait item again");
			undo();
			moved = {70, 90};
			obs_sceneitem_set_pos(mainItem, &moved);
			obs_data_set_int(changed, "width", 360);
			obs_source_update(sharedSource, changed);
			QTest::qWait(150);
			check(state(portrait()) == before,
			      "removed portrait reference restores its ID order and transform");
			select(sharedName);
			before = state(portrait());
			auto *list = get<QListWidget>(window, "verticalPreviewLayerList");
			list->setFocus();
			QTest::keyClick(list, Qt::Key_Space);
			QTest::qWait(350);
			after = state(portrait());
			check(!obs_sceneitem_visible(item()),
			      "auxiliary checkbox immediately hides only the portrait item");
			cycle(before, after);
			check(obs_sceneitem_visible(mainItem), "portrait visibility does not hide the Main reference");
			undo();
			select(sharedName);
			before = state(portrait());
			int n = count(portrait());
			addMenu(obs_source_get_uuid(sharedSource));
			after = state(portrait());
			check(count(portrait()) == n + 1, "Add source menu inserts one reference");
			cycle(before, after);
			undo();
			before = state(portrait());
			addMenu("whole");
			after = state(portrait());
			check(count(portrait()) == n + 1, "Add whole Main scene is undoable as a nested reference");
			cycle(before, after);
			undo();
			OBSScene oldPortrait = portrait();
			before = state(oldPortrait);
			history = get<QAction>(window, "actionMainUndo")->text();
			addMenu(obs_source_get_uuid(sharedSource), true);
			check(state(oldPortrait) == before && get<QAction>(window, "actionMainUndo")->text() == history,
			      "an Add menu opened for another scene cannot mutate the new scene or add Undo");
			go(mainSource);
			before = state(portrait());
			framing(true);
			after = state(portrait());
			check(info(mirror()).bounds_type == OBS_BOUNDS_SCALE_INNER &&
				      after["fit"].toInt() ==
					      get<QComboBox>(window, "verticalPreviewFraming")->currentData().toInt() &&
				      after["has_fit"].toBool(),
			      "mirror framing changes immediately and stores a per-scene preference");
			cycle(before, after);
			auto m = mirror();
			obs_sceneitem_set_locked(m, true);
			QTest::qWait(250);
			check(!get<QComboBox>(window, "verticalPreviewFraming")->isEnabled(),
			      "locked mirror disables the framing control");
			select(obs_source_get_name(obs_sceneitem_get_source(m)));
			check(!button("fit")->isEnabled(), "locked layer disables Fit");
			history = get<QAction>(window, "actionMainUndo")->text();
			get<QComboBox>(window, "verticalPreviewFraming")->setCurrentIndex(0);
			QTest::qWait(200);
			check(info(mirror()).bounds_type == OBS_BOUNDS_SCALE_INNER &&
				      get<QAction>(window, "actionMainUndo")->text() == history,
			      "a forced framing event cannot change a locked mirror or history");
			obs_sceneitem_set_locked(mirror(), false);
			QTest::qWait(150);
			before = state(portrait());
			click(button("mirror"));
			after = state(portrait());
			check(count(portrait()) == 1 && info(mirror()).bounds_type == OBS_BOUNDS_SCALE_INNER,
			      "Restore mirror replaces the graph and uses this scene's framing");
			cycle(before, after);
			undo();
			OBSSceneAutoRelease second = obs_scene_create("Portrait Commands QA second");
			go(obs_scene_get_source(second));
			check(info(mirror()).bounds_type == OBS_BOUNDS_SCALE_OUTER,
			      "a new scene does not inherit another scene's framing preference");
			go(mainSource);
			// The group and child become genuinely missing, exercising dependency recreation.
			QString groupUuid, childUuid;
			int64_t groupId = 0, childId = 0;
			{
				OBSSceneItem group = obs_scene_add_group2(portrait(), groupName, true);
				groupId = obs_sceneitem_get_id(group);
				groupUuid = obs_source_get_uuid(obs_sceneitem_get_source(group));
				OBSDataAutoRelease d = obs_data_create();
				obs_data_set_int(d, "width", 222);
				obs_data_set_int(d, "height", 111);
				obs_data_set_int(d, "color", 0xff22aa77);
				OBSSourceAutoRelease child = obs_source_create("color_source_v3", onlyName, d, nullptr);
				childUuid = obs_source_get_uuid(child);
				OBSDataAutoRelease fx = obs_data_create();
				obs_data_set_double(fx, "brightness", .12);
				OBSSourceAutoRelease filter = obs_source_create(
					"color_filter_v2", "Portrait Commands QA correction", fx, nullptr);
				check(bool(filter), "serialized effect fixture is available");
				obs_source_filter_add(child, filter);
				auto *childItem = obs_scene_add(obs_sceneitem_group_get_scene(group), child);
				childId = obs_sceneitem_get_id(childItem);
				vec2 position{12, 34};
				obs_sceneitem_set_pos(childItem, &position);
			}
			QTest::qWait(200);
			vec2 settledChildPosition{};
			{
				auto *childItem = obs_scene_find_sceneitem_by_id(
					obs_sceneitem_group_get_scene(item(groupName)), childId);
				obs_sceneitem_get_pos(childItem, &settledChildPosition);
			}
			select(groupName);
			before = state(portrait());
			click(button("remove"));
			after = state(portrait());
			QTest::qWait(500);
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			{
				OBSSourceAutoRelease gone = obs_get_source_by_uuid(groupUuid.toUtf8().constData()),
						     child = obs_get_source_by_uuid(childUuid.toUtf8().constData());
				check(!gone && !child,
				      "removed group and portrait-only child have actually been released");
			}
			undo();
			check(state(portrait()) == before && item(groupName) &&
				      obs_sceneitem_get_id(item(groupName)) == groupId,
			      "Undo recreates the missing group with the original item ID");
			{
				OBSSourceAutoRelease child = obs_get_source_by_uuid(childUuid.toUtf8().constData());
				auto *childItem = obs_scene_find_sceneitem_by_id(
					obs_sceneitem_group_get_scene(item(groupName)), childId);
				check(child && childItem && obs_source_get_width(child) == 222 &&
					      info(childItem).pos.x == settledChildPosition.x &&
					      info(childItem).pos.y == settledChildPosition.y,
				      "Undo restores missing child identity settings and transform");
				check(obs_source_filter_count(child) == 1,
				      "Undo recreates the child's serialized filter");
				OBSSourceAutoRelease filter =
					obs_source_get_filter_by_name(child, "Portrait Commands QA correction");
				OBSDataAutoRelease fx = obs_source_get_settings(filter);
				check(std::abs(obs_data_get_double(fx, "brightness") - .12) < .0001,
				      "recreated filter retains its saved parameters");
			}
			redo();
			check(state(portrait()) == after && !item(groupName),
			      "Redo removes a recreated group without duplicates");
			undo();
			check(state(mainScene) == mainInitial,
			      "all dock commands preserve the complete Main composition");
			check(rootFilterLoads.load() == 0, "Undo and Redo do not reload existing composition filters");
			OBSSourceAutoRelease program = obs_frontend_get_current_scene();
			check(program.Get() == mainSource.Get(),
			      "dock Undo and Redo never put Portrait on Main program");
			select(sharedName);
			before = state(portrait());
			list = get<QListWidget>(window, "verticalPreviewLayerList");
			list->setFocus();
			QTest::keyClick(list, Qt::Key_Space);
			QTest::qWait(350);
			check(!obs_sceneitem_visible(item()), "final visibility edit is saved for restart");
			saved["final"] = state(portrait());
			saved["portrait_uuid"] = obs_source_get_uuid(obs_scene_get_source(portrait()));
			saved["child_uuid"] = childUuid;
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
	       {"scope",
		"Native Linux Qt dock buttons, menu actions, native history, source graph recovery and full restart persistence. Shared inputs and Main are checked independently. Windows execution pending."}});
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
	root = qEnvironmentVariable("OBS_STUDIO_PORTRAIT_COMMANDS_ROOT");
	restore = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_COMMANDS_RESTORE");
	if (root.isEmpty()) {
		return false;
	}
	registerLoadProbe();
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
