/* Native, opt-in regression of immediate canvas actions and the shared source
 * tree/inspector. Run once normally and again with RESTORE=1 after restarting. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTest>
#include <QTimer>
#include <functional>
#include <stdexcept>
OBS_DECLARE_MODULE()
namespace {
QString root;
QMainWindow *window;
QJsonArray checks;
QJsonObject saved;
bool restore = false;
OBSOutputAutoRelease busyOutput;
OBSEncoderAutoRelease busyEncoder;
OBSEncoderAutoRelease busyAudio;
template<class T> T *get(QObject *p, const char *name)
{
	auto *item = p->findChild<T *>(name);
	if (!item) {
		throw std::runtime_error(name);
	}
	return item;
}
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}});
	blog(LOG_INFO, "[canvas-context-check] %s: %s", pass ? "PASS" : "FAIL", name);
	if (!pass) {
		throw std::runtime_error(name);
	}
}
void write(QString file, QJsonObject data)
{
	QFile f(root + "/" + file);
	if (!f.open(QIODevice::WriteOnly)) {
		throw std::runtime_error("report path unavailable");
	}
	f.write(QJsonDocument(data).toJson());
}
void mode(bool multi)
{
	bool ok = false;
	check(QMetaObject::invokeMethod(window, "SetStudioCanvasMode", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
					Q_ARG(bool, multi)) &&
		      ok,
	      "canvas mode action succeeds");
	QTest::qWait(200);
}
OBSCanvas vertical()
{
	return OBSCanvasAutoRelease{obs_get_canvas_by_name("Vertical 9:16")}.Get();
}
OBSScene portrait()
{
	OBSCanvas canvas = vertical();
	OBSSourceAutoRelease source = canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
	return source ? obs_scene_from_source(source) : nullptr;
}
void editing(bool portrait)
{
	auto *combo = get<QComboBox>(window, "studioActiveCanvas");
	combo->setFocus();
	QTest::keyClick(combo, portrait ? Qt::Key_End : Qt::Key_Home);
	QTest::qWait(200);
	check(window->property("studioEditingCanvas").toString() == (portrait ? "portrait" : "main"),
	      "active canvas selector controls editing context");
}
int row(const QString &name)
{
	auto *tree = get<QListView>(window, "sources");
	for (int i = 0; i < tree->model()->rowCount(); ++i) {
		if (tree->model()->index(i, 0).data(Qt::AccessibleTextRole).toString() == name) {
			return i;
		}
	}
	return -1;
}
void select(const QString &name)
{
	auto *tree = get<QListView>(window, "sources");
	const int index = row(name);
	check(index >= 0, "shared source tree contains the requested canvas layer");
	auto at = tree->model()->index(index, 0);
	tree->scrollTo(at);
	QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(at).center());
	QTest::qWait(350);
	check(get<QLabel>(window, "studioSelectedSourceName")->text() == name,
	      "inspector follows the selected layer in the shared source tree");
}
void number(const char *name, const QString &value)
{
	auto *spin = get<QDoubleSpinBox>(window, name);
	spin->setFocus();
	QTest::keyClick(spin, Qt::Key_A, Qt::ControlModifier);
	QTest::keyClicks(spin, value);
	QTest::keyClick(spin, Qt::Key_Return);
	QTest::qWait(180);
}
void click(QPushButton *button)
{
	for (auto *p = button->parentWidget(); p; p = p->parentWidget()) {
		if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
			scroll->ensureWidgetVisible(button);
		}
	}
	QTest::mouseClick(button, Qt::LeftButton);
	QTest::qWait(180);
}
void settings(std::function<void(QDialog *)> fn)
{
	QString failure;
	QTimer::singleShot(250, qApp, [&]() {
		auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
		try {
			check(d && d->objectName() == "OBSBasicSettings", "native settings opens");
			get<QListWidget>(d, "listWidget")->setCurrentRow(0);
			fn(d);
		} catch (const std::exception &e) {
			failure = QString::fromUtf8(e.what());
		}
		if (d) {
			get<QDialogButtonBox>(d, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
		}
	});
	get<QAction>(window, "action_Settings")->trigger();
	if (!failure.isEmpty()) {
		throw std::runtime_error(failure.toStdString());
	}
}
vec2 position(obs_sceneitem_t *item)
{
	vec2 p{};
	obs_sceneitem_get_pos(item, &p);
	return p;
}
int references(obs_scene_t *scene, obs_source_t *source)
{
	struct Count {
		obs_source_t *source;
		int count = 0;
	} c{source};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			auto *c = static_cast<Count *>(data);
			if (obs_sceneitem_get_source(item) == c->source) {
				++c->count;
			}
			return true;
		},
		&c);
	return c.count;
}
void cleanup()
{
	if (busyOutput) {
		if (obs_output_active(busyOutput)) {
			obs_output_force_stop(busyOutput);
		}
		for (int i = 0; i < 100 && obs_output_active(busyOutput); ++i) {
			QTest::qWait(100);
		}
		busyOutput = nullptr;
		busyEncoder = nullptr;
		busyAudio = nullptr;
	}
	if (saved.isEmpty()) {
		return;
	}
	QMetaObject::invokeMethod(window, "SetStudioPortraitEditing", Qt::DirectConnection, Q_ARG(bool, false));
	OBSSourceAutoRelease original = obs_get_source_by_uuid(saved["scene"].toString().toUtf8().constData());
	if (original) {
		obs_frontend_set_current_scene(original);
	}
	OBSSourceAutoRelease fixture = obs_get_source_by_name("Canvas Context QA");
	if (fixture) {
		obs_source_remove(fixture);
	}
	for (const char *name : {"Canvas Context QA shared", "Canvas Context QA portrait only"}) {
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
	if (saved["studio_mode"].toBool()) {
		obs_frontend_set_preview_program_mode(true);
		OBSSourceAutoRelease preview = obs_get_source_by_uuid(saved["preview"].toString().toUtf8().constData());
		if (preview) {
			obs_frontend_set_current_preview_scene(preview);
		}
	}
	QTest::qWait(300);
	window->restoreState(QByteArray::fromBase64(saved["layout"].toString().toUtf8()));
	obs_frontend_save();
}
void run()
{
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && !obs_video_active(), "canvas context regression begins idle");
		if (restore) {
			QFile f(root + "/saved.json");
			check(f.open(QIODevice::ReadOnly), "private saved baseline available after restart");
			saved = QJsonDocument::fromJson(f.readAll()).object();
		} else {
			OBSSourceAutoRelease original = obs_frontend_get_current_scene(),
					     preview = obs_frontend_get_current_preview_scene();
			auto *u = obs_frontend_get_user_config();
			saved = {{"scene", obs_source_get_uuid(original)},
				 {"preview", preview ? obs_source_get_uuid(preview) : ""},
				 {"had_multi", config_has_user_value(u, "BasicWindow", "StudioMultiCanvas")},
				 {"multi", !config_has_user_value(u, "BasicWindow", "StudioMultiCanvas") ||
						   config_get_bool(u, "BasicWindow", "StudioMultiCanvas")},
				 {"studio_mode", obs_frontend_preview_program_mode_active()},
				 {"layout", QString::fromUtf8(window->saveState().toBase64())}};
			write("saved.json", saved);
			obs_frontend_set_preview_program_mode(false);
			mode(true);
			OBSSceneAutoRelease fixture = obs_scene_create("Canvas Context QA");
			OBSDataAutoRelease d = obs_data_create();
			obs_data_set_int(d, "width", 320);
			obs_data_set_int(d, "height", 240);
			obs_data_set_int(d, "color", 0xff3344bb);
			OBSSourceAutoRelease source =
				obs_source_create("color_source_v3", "Canvas Context QA shared", d, nullptr);
			auto *item = obs_scene_add(fixture, source);
			vec2 p{40, 50};
			obs_sceneitem_set_pos(item, &p);
			obs_frontend_set_current_scene(obs_scene_get_source(fixture));
			QTest::qWait(500);
			OBSScene scene = portrait();
			check(bool(scene), "fixture has a persistent linked portrait scene");
			auto *copy = obs_scene_add(scene, source);
			p = {70, 80};
			obs_sceneitem_set_pos(copy, &p);
			obs_data_set_int(d, "width", 160);
			OBSSourceAutoRelease only =
				obs_source_create("color_source_v3", "Canvas Context QA portrait only", d, nullptr);
			obs_scene_add(scene, only);
		}
		mode(true);
		get<QDockWidget>(window, "sourcesDock")->show();
		get<QDockWidget>(window, "studioInspectorDock")->show();
		get<QDockWidget>(window, "studioInspectorDock")->raise();
		OBSScene scene = portrait();
		OBSSceneItem copy = obs_scene_find_source(scene, "Canvas Context QA shared");
		OBSSourceAutoRelease fixtureSource = obs_get_source_by_name("Canvas Context QA");
		OBSSceneItem mainItem =
			obs_scene_find_source(obs_scene_from_source(fixtureSource), "Canvas Context QA shared");
		check(copy && mainItem, "shared source exists in two independent scene items");
		check(obs_sceneitem_get_source(copy) == obs_sceneitem_get_source(mainItem),
		      "both canvases reuse the same capture/source instance");
		if (restore) {
			check(QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(scene))) ==
				      saved["portrait_uuid"].toString(),
			      "restart reattaches the same persisted Portrait scene UUID");
			check(position(copy).x == 333 && position(mainItem).x == 40,
			      "portrait transform survives restart without altering Main");
			editing(true);
			select("Canvas Context QA shared");
			check(get<QDoubleSpinBox>(window, "studioPositionX")->value() == 333,
			      "inspector reloads persisted portrait transform");
			cleanup();
		} else {
			const QString uuid = obs_source_get_uuid(obs_scene_get_source(scene));
			settings([](QDialog *d) { click(get<QPushButton>(d, "studioCanvasSingle")); });
			check(!portrait() && !get<QDockWidget>(window, "verticalPreviewDock")->isVisible(),
			      "Main card stops portrait program and hides preview without Apply, including Cancel");
			settings([](QDialog *d) {
				check(get<QPushButton>(d, "studioCanvasSingle")->isChecked(),
				      "reopening settings reflects the immediate Main state");
				click(get<QPushButton>(d, "studioCanvasMulti"));
				check(bool(portrait()),
				      "Multi card immediately restores portrait program before Apply");
			});
			check(get<QDockWidget>(window, "verticalPreviewDock")->isVisible(),
			      "Cancel does not revert the immediate Multi action");
			check(QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(portrait()))) == uuid,
			      "mode toggles preserve portrait scene UUID");
			editing(false);
			check(row("Canvas Context QA portrait only") < 0,
			      "Main layer list excludes portrait-only layers");
			editing(true);
			check(row("Canvas Context QA portrait only") >= 0,
			      "Portrait layer list includes portrait-only layers");
			editing(false);
			select("Canvas Context QA shared");
			get<QAction>(window, "actionCopySource")->trigger();
			editing(true);
			get<QAction>(window, "actionPasteRef")->trigger();
			QTest::qWait(300);
			check(references(portrait(), obs_sceneitem_get_source(mainItem)) == 2,
			      "native Paste Reference adds to Portrait rather than Main");
			get<QAction>(window, "actionMainUndo")->trigger();
			QTest::qWait(300);
			check(references(portrait(), obs_sceneitem_get_source(mainItem)) == 1 &&
				      references(obs_scene_from_source(fixtureSource),
						 obs_sceneitem_get_source(mainItem)) == 1,
			      "Undo reference insertion preserves the original Main and Portrait items");
			// Native snapshot Undo reconstructs scene items, preserving IDs rather than pointers.
			copy = obs_scene_find_source(portrait(), "Canvas Context QA shared");
			mainItem =
				obs_scene_find_source(obs_scene_from_source(fixtureSource), "Canvas Context QA shared");
			select("Canvas Context QA shared");
			number("studioPositionX", "333");
			check(position(copy).x == 333 && position(mainItem).x == 40,
			      "inspector changes only the portrait scene item");
			get<QAction>(window, "actionMainUndo")->trigger();
			QTest::qWait(300);
			check(position(copy).x == 70 && position(mainItem).x == 40,
			      "Undo restores only the portrait transform");
			get<QAction>(window, "actionMainRedo")->trigger();
			QTest::qWait(300);
			check(position(copy).x == 333, "Redo restores the portrait transform");
			OBSSourceAutoRelease program = obs_frontend_get_current_scene();
			check(program.Get() == fixtureSource.Get(),
			      "editing and undo never replace the Main program with a portrait scene");
			get<QAction>(window, "actionSourceUp")->trigger();
			QTest::qWait(300);
			get<QAction>(window, "actionMainUndo")->trigger();
			QTest::qWait(350);
			OBSSourceAutoRelease retained = obs_get_source_by_name("Canvas Context QA");
			check(bool(retained), "native reorder Undo keeps the original Main scene");
			select("Canvas Context QA portrait only");
			QSpinBox *width = nullptr;
			for (auto *spin : get<QWidget>(window, "studioInlineProperties")->findChildren<QSpinBox *>()) {
				if (spin->minimum() == 1 && spin->maximum() == 4096 && spin->value() == 160) {
					width = spin;
				}
			}
			check(width != nullptr,
			      "native integer property control is available in the Portrait inspector");
			width->setFocus();
			QTest::keyClick(width, Qt::Key_A, Qt::ControlModifier);
			QTest::keyClicks(width, "176");
			QTest::keyClick(width, Qt::Key_Return);
			QTest::qWait(500);
			OBSSourceAutoRelease only = obs_get_source_by_name("Canvas Context QA portrait only");
			check(obs_source_get_width(only) == 176,
			      "inline source properties edit the selected Portrait source");
			// Native OBS coalesces property Undo on a 500 ms Qt timer.
			for (int i = 0;
			     i < 50 &&
			     !get<QAction>(window, "actionMainUndo")->text().contains("Canvas Context QA portrait only");
			     ++i) {
				QTest::qWait(50);
			}
			check(get<QAction>(window, "actionMainUndo")->text().contains("Canvas Context QA portrait only"),
			      "native property Undo action has finished coalescing");
			get<QAction>(window, "actionMainUndo")->trigger();
			QTest::qWait(350);
			check(obs_source_get_width(only) == 160, "Undo restores inline Portrait source properties");
			click(get<QPushButton>(window, "studioSourceProperties"));
			QTest::qWait(150);
			for (auto *d : window->findChildren<QDialog *>()) {
				if (d->isVisible()) {
					d->close();
				}
			}
			click(get<QPushButton>(window, "studioSourceTransform"));
			QTest::qWait(150);
			auto *advanced = window->findChild<QDialog *>("OBSBasicTransform");
			check(advanced && advanced->isVisible(), "advanced transform opens for the Portrait item");
			editing(false);
			QTest::qWait(100);
			check(!window->findChild<QDialog *>("OBSBasicTransform") ||
				      !window->findChild<QDialog *>("OBSBasicTransform")->isVisible(),
			      "switching canvas closes the old advanced editor instead of retargeting Reset");
			select("Canvas Context QA shared");
			check(get<QDoubleSpinBox>(window, "studioPositionX")->value() == 40,
			      "Main inspector recovers its independent transform");
			editing(true);
			select("Canvas Context QA shared");
			OBSDataAutoRelease encoderSettings = obs_data_create(), outputSettings = obs_data_create();
			obs_data_set_int(encoderSettings, "bitrate", 1000);
			obs_data_set_string(encoderSettings, "preset", "ultrafast");
			busyEncoder =
				obs_video_encoder_create("obs_x264", "canvas_context_busy", encoderSettings, nullptr);
			OBSCanvas canvas = vertical();
			obs_encoder_set_video(busyEncoder, obs_canvas_get_video(canvas));
			obs_data_set_string(outputSettings, "path",
					    (root + "/portrait-ui-check.mkv").toUtf8().constData());
			busyOutput = obs_output_create("ffmpeg_muxer", "canvas_context_busy", outputSettings, nullptr);
			busyAudio = obs_audio_encoder_create("ffmpeg_aac", "canvas_context_busy_audio", nullptr, 0,
							     nullptr);
			if (busyAudio) {
				obs_encoder_set_audio(busyAudio, obs_get_audio());
			}
			check(busyEncoder && busyOutput && busyAudio, "local encoded busy fixture created");
			obs_output_set_video_encoder(busyOutput, busyEncoder);
			obs_output_set_audio_encoder(busyOutput, busyAudio, 0);
			check(obs_output_start(busyOutput), "portrait fixture encodes real video for the busy guard");
			QTest::qWait(400);
			bool accepted = true;
			QMetaObject::invokeMethod(window, "SetStudioCanvasMode", Qt::DirectConnection,
						  Q_RETURN_ARG(bool, accepted), Q_ARG(bool, false));
			check(!accepted && bool(portrait()),
			      "active encoder rejects mode changes without changing portrait program");
			settings([](QDialog *d) {
				check(!get<QPushButton>(d, "studioCanvasSingle")->isEnabled() &&
					      !get<QPushButton>(d, "studioCanvasMulti")->isEnabled(),
				      "both mode cards are disabled while encoding");
			});
			obs_output_force_stop(busyOutput);
			for (int i = 0; i < 100 && obs_output_active(busyOutput); ++i) {
				QTest::qWait(100);
			}
			check(!obs_output_active(busyOutput), "test encoder stops before handoff and restart");
			busyOutput = nullptr;
			busyEncoder = nullptr;
			busyAudio = nullptr;
			saved["portrait_uuid"] =
				QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(portrait())));
			write("saved.json", saved);
			obs_frontend_save();
		}
	} catch (const std::exception &e) {
		error = QString::fromUtf8(e.what());
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
		"Native Linux immediate Main/Multi actions, shared source tree and inspector, independent transforms, Undo/Redo and restart persistence. Windows native execution pending."}});
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(1500, qApp, run);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_CANVAS_CONTEXT_ROOT");
	restore = qEnvironmentVariableIsSet("OBS_STUDIO_CANVAS_CONTEXT_RESTORE");
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
