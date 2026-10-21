/* Opt-in native program-transition check. Only disposable local color scenes;
 * no account, capture device or streaming destination is used. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <graphics/graphics.h>
#include <util/config-file.h>
#include <QApplication>
#include <QComboBox>
#include <QSpinBox>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QTest>
#include <QTimer>
#include <array>
#include <functional>
#include <stdexcept>
#include <vector>

OBS_DECLARE_MODULE()
namespace {
constexpr const char *modeKey = "StudioPortraitTransition";
constexpr const char *durationKey = "StudioPortraitTransitionDuration";
constexpr const char *nameA = "Portrait Transition QA A";
constexpr const char *nameB = "Portrait Transition QA B";
constexpr const char *redName = "Portrait Transition QA red";
constexpr const char *blueName = "Portrait Transition QA blue";
QString root;
QMainWindow *window;
QObject *controller;
QJsonObject saved;
QJsonArray checks;
bool restore;
QTimer *startTimer;
bool started = false;

void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name}, {"passed", pass}});
	blog(LOG_INFO, "[portrait-transitions-check] %s %s", pass ? "PASS" : "FAIL", name);
	if (!pass) {
		throw std::runtime_error(name);
	}
}
bool wait(const std::function<bool()> &predicate, int timeout = 4000)
{
	for (int ms = 0; ms < timeout; ms += 20) {
		if (predicate()) {
			return true;
		}
		QTest::qWait(20);
	}
	return predicate();
}
void write(const char *name, QJsonObject data)
{
	QFile file(root + "/" + name);
	if (!file.open(QIODevice::WriteOnly)) {
		throw std::runtime_error("report unavailable");
	}
	file.write(QJsonDocument(data).toJson());
}
int setting(const char *method)
{
	int value = -1;
	if (!QMetaObject::invokeMethod(controller, method, Qt::DirectConnection, Q_RETURN_ARG(int, value))) {
		throw std::runtime_error(method);
	}
	return value;
}
bool setTransition(int mode, int duration)
{
	bool ok = false;
	check(QMetaObject::invokeMethod(controller, "SetPortraitTransition", Qt::DirectConnection,
					Q_RETURN_ARG(bool, ok), Q_ARG(int, mode), Q_ARG(int, duration)),
	      "native transition setter is invokable");
	return ok;
}
void multi(bool enabled)
{
	bool ok = false;
	check(QMetaObject::invokeMethod(window, "SetStudioCanvasMode", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
					Q_ARG(bool, enabled)) &&
		      ok,
	      "native canvas mode action succeeds");
}
OBSCanvas canvas()
{
	OBSCanvasAutoRelease value = obs_get_canvas_by_name("Vertical 9:16");
	return value.Get();
}
OBSSource program()
{
	OBSCanvas c = canvas();
	OBSSourceAutoRelease s = c ? obs_canvas_get_channel(c, 0) : nullptr;
	return s.Get();
}
OBSScene portrait()
{
	OBSSource s = program();
	if (s && obs_source_get_type(s) == OBS_SOURCE_TYPE_TRANSITION) {
		OBSSourceAutoRelease target = obs_transition_get_active_source(s);
		return obs_scene_from_source(target);
	}
	return obs_scene_from_source(s);
}
bool linked(obs_source_t *main)
{
	OBSScene scene = portrait();
	OBSDataAutoRelease priv = scene ? obs_source_get_private_settings(obs_scene_get_source(scene)) : nullptr;
	return main && scene &&
	       QString::fromUtf8(obs_data_get_string(priv, "otaku_main_scene_uuid")) == obs_source_get_uuid(main);
}
void go(obs_source_t *scene)
{
	obs_frontend_set_current_scene(scene);
	check(wait([&]() { return linked(scene); }), "Portrait reaches the requested Main scene by UUID");
}
OBSSource transition(const char *id)
{
	obs_frontend_source_list list{};
	obs_frontend_get_transitions(&list);
	OBSSource found;
	for (size_t i = 0; i < list.sources.num; ++i) {
		if (QString::fromUtf8(obs_source_get_unversioned_id(list.sources.array[i])) == id) {
			found = list.sources.array[i];
		}
	}
	obs_frontend_source_list_free(&list);
	return found;
}
void settle()
{
	check(wait(
		      []() {
			      OBSSource p = program();
			      OBSSourceAutoRelease main = obs_get_output_source(0);
			      return (!p || obs_source_get_type(p) != OBS_SOURCE_TYPE_TRANSITION ||
				      !obs_transition_is_active(p)) &&
				     (!main || !obs_transition_is_active(main));
		      },
		      22000),
	      "both program transitions settle before the next independent case");
}
std::array<int, 4> pixel()
{
	std::array<int, 4> result{-1, -1, -1, -1};
	OBSCanvas c = canvas();
	obs_enter_graphics();
	gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(2, 2, GS_RGBA);
	if (render && stage && gs_texrender_begin(render, 2, 2)) {
		vec4 clear{};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
		gs_ortho(0.f, 1080.f, 0.f, 1920.f, -100.f, 100.f);
		obs_render_canvas_texture(c);
		gs_texrender_end(render);
		gs_stage_texture(stage, gs_texrender_get_texture(render));
		uint8_t *data = nullptr;
		uint32_t stride = 0;
		if (gs_stagesurface_map(stage, &data, &stride)) {
			for (int i = 0; i < 4; ++i) {
				result[i] = data[i];
			}
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(render);
	obs_leave_graphics();
	return result;
}
OBSSource color(const char *name, uint32_t value)
{
	obs_video_info video{};
	if (!obs_get_video_info(&video)) {
		return nullptr;
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "color", value);
	obs_data_set_int(settings, "width", video.base_width);
	obs_data_set_int(settings, "height", video.base_height);
	OBSSourceAutoRelease source = obs_source_create("color_source_v3", name, settings, nullptr);
	return source.Get();
}
void restoreKey(config_t *config, const char *key, const char *had, const char *value)
{
	if (saved[had].toBool()) {
		config_set_int(config, "BasicWindow", key, saved[value].toInt());
	} else {
		config_remove_value(config, "BasicWindow", key);
	}
}
void cleanup()
{
	if (saved.isEmpty()) {
		return;
	}
	setTransition(0, 300);
	OBSSource cut = transition("cut_transition");
	if (cut) {
		obs_frontend_set_current_transition(cut);
	}
	OBSSourceAutoRelease original = obs_get_source_by_uuid(saved["scene"].toString().toUtf8().constData());
	if (original) {
		go(original);
	}
	QMetaObject::invokeMethod(window, "SetStudioPortraitEditing", Qt::DirectConnection, Q_ARG(bool, false));
	for (const char *name : {nameA, nameB, redName, blueName}) {
		OBSSourceAutoRelease source = obs_get_source_by_name(name);
		if (source) {
			obs_source_remove(source);
		}
	}
	// Remove only our now-inactive Portrait compositions; never migrate canvases.
	OBSCanvas c = canvas();
	std::vector<OBSSource> ownScenes;
	obs_canvas_enum_scenes(
		c,
		[](void *data, obs_source_t *source) {
			const QString name = QString::fromUtf8(obs_source_get_name(source));
			if (name.startsWith(nameA) || name.startsWith(nameB)) {
				static_cast<std::vector<OBSSource> *>(data)->emplace_back(source);
			}
			return true;
		},
		&ownScenes);
	for (auto &s : ownScenes) {
		obs_source_remove(s);
	}
	OBSSourceAutoRelease oldTransition =
		obs_get_source_by_uuid(saved["main_transition"].toString().toUtf8().constData());
	if (oldTransition) {
		obs_frontend_set_current_transition(oldTransition);
	}
	obs_frontend_set_transition_duration(saved["main_duration"].toInt());
	config_t *config = obs_frontend_get_user_config();
	restoreKey(config, modeKey, "had_mode", "mode");
	restoreKey(config, durationKey, "had_duration", "duration");
	multi(saved["multi"].toBool());
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
	QTest::qWait(200);
	window->restoreState(QByteArray::fromBase64(saved["layout"].toString().toUtf8()));
	obs_frontend_save();
}
void run()
{
	if (started) {
		return;
	}
	started = true;
	QString error;
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		controller = window->findChild<QObject *>("studioDualCanvasController");
		check(controller, "production DualCanvasController is discoverable");
		check(!obs_video_active() && !obs_frontend_virtualcam_active(), "test starts only with idle outputs");
		QFile previous(root + "/saved.json");
		if (previous.open(QIODevice::ReadOnly)) {
			saved = QJsonDocument::fromJson(previous.readAll()).object();
		}
		if (!restore && !saved.isEmpty()) {
			cleanup();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			saved = {};
		}
		if (!restore) {
			OBSSourceAutoRelease original = obs_frontend_get_current_scene();
			OBSSourceAutoRelease preview = obs_frontend_get_current_preview_scene();
			OBSSourceAutoRelease mainTransition = obs_frontend_get_current_transition();
			config_t *config = obs_frontend_get_user_config();
			saved = {{"scene", obs_source_get_uuid(original)},
				 {"preview", preview ? obs_source_get_uuid(preview) : ""},
				 {"studio", obs_frontend_preview_program_mode_active()},
				 {"main_transition", mainTransition ? obs_source_get_uuid(mainTransition) : ""},
				 {"main_duration", obs_frontend_get_transition_duration()},
				 {"had_mode", config_has_user_value(config, "BasicWindow", modeKey)},
				 {"had_duration", config_has_user_value(config, "BasicWindow", durationKey)},
				 {"mode", int(config_get_int(config, "BasicWindow", modeKey))},
				 {"duration", int(config_get_int(config, "BasicWindow", durationKey))},
				 {"multi", !config_has_user_value(config, "BasicWindow", "StudioMultiCanvas") ||
						   config_get_bool(config, "BasicWindow", "StudioMultiCanvas")},
				 {"had_multi", config_has_user_value(config, "BasicWindow", "StudioMultiCanvas")},
				 {"layout", QString::fromUtf8(window->saveState().toBase64())}};
			write("saved.json", saved);
		}
		if (obs_frontend_preview_program_mode_active()) {
			obs_frontend_set_preview_program_mode(false);
		}
		multi(true);
		check((obs_canvas_get_flags(canvas()) & MIX_AUDIO) == 0,
		      "Portrait canvas does not mix a second audio bus");
		OBSSource cut = transition("cut_transition"), fade = transition("fade_transition");
		check(cut && fade, "common built-in Main transitions are available");
		obs_frontend_set_current_transition(cut);
		if (restore) {
			check(!saved.isEmpty(), "restart has its own preserved baseline");
			check(setting("PortraitTransitionModeValue") == 1 &&
				      setting("PortraitTransitionDuration") == 1300,
			      "independent Fade mode and duration survive a full process restart");
			auto *combo = window->findChild<QComboBox *>("studioPortraitTransitionMode");
			auto *spin = window->findChild<QSpinBox *>("studioPortraitTransitionDuration");
			check(combo && spin && combo->currentData().toInt() == 1 && spin->value() == 1300,
			      "dock controls restore the persisted mode and duration without Apply");
			OBSSourceAutoRelease a = obs_get_source_by_name(nameA), b = obs_get_source_by_name(nameB);
			check(a && b, "disposable compositions survive restart");
			check(QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(portrait()))) ==
				      saved["portrait_uuid"].toString(),
			      "the same Portrait root UUID is restored, without duplicate canvas creation");
			go(b);
			OBSSource p = program();
			check(p && obs_source_get_type(p) == OBS_SOURCE_TYPE_TRANSITION && obs_transition_is_active(p),
			      "restored preference really drives the next scene transition");
			settle();
			cleanup();
			check(setting("PortraitTransitionModeValue") == saved["mode"].toInt(),
			      "original transition mode is restored");
		} else {
			auto *combo = window->findChild<QComboBox *>("studioPortraitTransitionMode");
			auto *spin = window->findChild<QSpinBox *>("studioPortraitTransitionDuration");
			check(combo && spin, "dock provides transition mode and duration controls");
			combo->setCurrentIndex(combo->findData(1));
			spin->setValue(850);
			check(setting("PortraitTransitionModeValue") == 1 &&
				      setting("PortraitTransitionDuration") == 850 && spin->isEnabled(),
			      "changing dock controls immediately saves independent Fade and duration without Apply");
			combo->setCurrentIndex(combo->findData(2));
			check(setting("PortraitTransitionModeValue") == 2 && !spin->isEnabled(),
			      "Follow Main disables the independent duration field");
			combo->setCurrentIndex(combo->findData(0));
			check(setting("PortraitTransitionModeValue") == 0 && !spin->isEnabled(),
			      "Cut disables the duration field");
			OBSSceneAutoRelease a = obs_scene_create(nameA), b = obs_scene_create(nameB);
			OBSSource red = color(redName, 0xff0000ff), blue = color(blueName, 0xffff0000);
			check(a && b && red && blue, "disposable color scenes are created");
			obs_scene_add(a, red);
			obs_scene_add(b, blue);
			check(setTransition(0, 300), "Cut preference saves immediately");
			go(obs_scene_get_source(a));
			settle();
			check(wait([]() {
				      auto p = pixel();
				      return p[0] > 230 && p[2] < 10;
			      }),
			      "actual Portrait texture starts red");
			check(setTransition(1, 1000), "independent Fade and duration save without Apply");
			check(!setTransition(3, 1000) && !setTransition(1, 49) && !setTransition(1, 20001),
			      "invalid modes and duration bounds are rejected");
			check(setting("PortraitTransitionModeValue") == 1 &&
				      setting("PortraitTransitionDuration") == 1000,
			      "rejected settings leave the effective preference unchanged");
			go(obs_scene_get_source(b));
			OBSSource p = program();
			check(p && obs_source_get_type(p) == OBS_SOURCE_TYPE_TRANSITION && obs_transition_is_active(p),
			      "independent Fade is active even when Main uses Cut");
			check(obs_source_get_width(p) == 1080 && obs_source_get_height(p) == 1920,
			      "private transition keeps portrait dimensions");
			QTest::qWait(300);
			const float progress = obs_transition_get_time(p);
			check(progress > .1f && progress < .7f,
			      "independent duration produces an intermediate backend frame");
			auto blend = pixel();
			write("blend.json", {{"progress", progress},
					     {"r", blend[0]},
					     {"g", blend[1]},
					     {"b", blend[2]},
					     {"a", blend[3]}});
			check(blend[0] > 20 && blend[2] > 20 && blend[1] < 10 && blend[3] > 230,
			      "actual rendered Portrait texture contains both red and blue during Fade");
			OBSSourceAutoRelease main = obs_frontend_get_current_scene();
			check(main == obs_scene_get_source(b),
			      "Portrait transition never replaces Main program with its private source");
			settle();
			check(wait([]() {
				      auto p = pixel();
				      return p[2] > 230 && p[0] < 10;
			      }),
			      "actual Portrait texture settles blue");
			check(setTransition(2, 1900), "Follow Main preference saves");
			obs_frontend_set_current_transition(fade);
			obs_frontend_set_transition_duration(1000);
			go(obs_scene_get_source(a));
			QTest::qWait(200);
			p = program();
			OBSSourceAutoRelease mainProgram = obs_get_output_source(0);
			write("follow-diagnostic.json",
			      {{"portrait_id", p ? obs_source_get_unversioned_id(p) : ""},
			       {"portrait_active", p && obs_source_get_type(p) == OBS_SOURCE_TYPE_TRANSITION &&
							   obs_transition_is_active(p)},
			       {"portrait_time", p && obs_source_get_type(p) == OBS_SOURCE_TYPE_TRANSITION
							 ? obs_transition_get_time(p)
							 : -1},
			       {"main_id", mainProgram ? obs_source_get_unversioned_id(mainProgram) : ""},
			       {"main_active", mainProgram && obs_transition_is_active(mainProgram)},
			       {"main_time", mainProgram ? obs_transition_get_time(mainProgram) : -1},
			       {"main_duration", obs_frontend_get_transition_duration()},
			       {"portrait_mode", setting("PortraitTransitionModeValue")}});
			check(p && obs_source_get_type(p) == OBS_SOURCE_TYPE_TRANSITION &&
				      obs_transition_is_active(p) && obs_transition_is_active(mainProgram),
			      "Follow Fade starts while Main is still transitioning");
			check(std::abs(obs_transition_get_time(p) - obs_transition_get_time(mainProgram)) < .15f,
			      "Follow Fade uses Main duration rather than independent duration");
			settle();
			obs_frontend_set_current_transition(cut);
			go(obs_scene_get_source(b));
			check(obs_source_get_type(program()) == OBS_SOURCE_TYPE_SCENE,
			      "Follow Cut restores a direct scene program");
			check(setTransition(0, 500), "independent Cut is selectable");
			obs_frontend_set_current_transition(fade);
			obs_frontend_set_transition_duration(1200);
			go(obs_scene_get_source(a));
			mainProgram = obs_get_output_source(0);
			check(obs_source_get_type(program()) == OBS_SOURCE_TYPE_SCENE &&
				      obs_transition_is_active(mainProgram),
			      "independent Cut changes Portrait at Main transition start, not at its completion");
			settle();
			OBSSourceAutoRelease unrelated = obs_source_create_private(
				"fade_transition", "Portrait Transition QA unrelated", nullptr);
			obs_transition_set(unrelated, obs_scene_get_source(a));
			obs_transition_start(unrelated, OBS_TRANSITION_MODE_AUTO, 300, obs_scene_get_source(b));
			QTest::qWait(100);
			check(linked(obs_scene_get_source(a)),
			      "item/private transition notifications cannot switch Portrait");
			obs_transition_clear(unrelated);
			check(setTransition(1, 700), "independent Fade is available in Studio Mode");
			obs_frontend_set_preview_program_mode(true);
			obs_frontend_set_current_preview_scene(obs_scene_get_source(b));
			QTest::qWait(150);
			check(linked(obs_scene_get_source(a)),
			      "changing only Studio Mode preview leaves Portrait on the actual program composition");
			obs_frontend_preview_program_trigger_transition();
			check(wait([&]() { return linked(obs_scene_get_source(b)); }),
			      "Studio Mode program transition selects the linked Portrait root");
			p = program();
			check(p && obs_source_get_type(p) == OBS_SOURCE_TYPE_TRANSITION && obs_transition_is_active(p),
			      "independent Portrait Fade starts during the Studio Mode program transition");
			settle();
			obs_frontend_set_current_preview_scene(obs_scene_get_source(a));
			QTest::qWait(150);
			check(linked(obs_scene_get_source(b)),
			      "distinct preview and program stay separate after the transition");
			obs_frontend_set_preview_program_mode(false);
			obs_frontend_set_current_transition(cut);
			check(setTransition(1, 1300), "final persistent preference is saved");
			go(obs_scene_get_source(b));
			QTest::qWait(80);
			go(obs_scene_get_source(a));
			settle();
			check(linked(obs_scene_get_source(a)),
			      "rapid program requests settle at the newest Portrait scene");
			{
				// Only this disposable root is removed. Keeping it strongly held
				// proves recovery ignores removed slots that have not been freed.
				check(!obs_video_active() && !obs_frontend_virtualcam_active(),
				      "removed-root recovery is exercised only with idle outputs");
				OBSScene removedRoot = portrait();
				OBSCanvas originalCanvas = canvas();
				check(removedRoot && originalCanvas,
				      "recovery fixture starts with a live Portrait root");
				const QString removedUuid = obs_source_get_uuid(obs_scene_get_source(removedRoot));
				const QString canvasUuid = obs_canvas_get_uuid(originalCanvas);
				saved["removed_portrait_uuid"] = removedUuid;
				obs_source_remove(obs_scene_get_source(removedRoot));
				check(obs_source_removed(obs_scene_get_source(removedRoot)),
				      "the disposable Portrait root is marked removed while still strongly retained");
				check(wait([&]() {
					      OBSScene replacement = portrait();
					      return replacement &&
						     !obs_source_removed(obs_scene_get_source(replacement)) &&
						     removedUuid !=
							     obs_source_get_uuid(obs_scene_get_source(replacement)) &&
						     linked(obs_scene_get_source(a));
				      }),
				      "root remove signal automatically replaces the removed root for the same Main scene");
				OBSScene replacement = portrait();
				const QString replacementUuid = obs_source_get_uuid(obs_scene_get_source(replacement));
				OBSCanvasAutoRelease owner = obs_source_get_canvas(obs_scene_get_source(replacement));
				check(owner == originalCanvas && canvas() == originalCanvas &&
					      canvasUuid == obs_canvas_get_uuid(canvas()),
				      "recovery assigns the replacement to the existing Portrait canvas without recreating it");
				check(program() == obs_scene_get_source(replacement),
				      "the program channel points directly at the valid replacement instead of a removed Fade target");
				check(wait([]() {
					      const auto p = pixel();
					      return p[0] > 230 && p[1] < 10 && p[2] < 10 && p[3] > 230;
				      }),
				      "the replacement default mirror really renders the red Main scene");
				OBSSourceAutoRelease sameMain = obs_frontend_get_current_scene();
				check(sameMain == obs_scene_get_source(a) && !obs_source_removed(sameMain),
				      "recovery leaves the original Main program source intact");
				check(QMetaObject::invokeMethod(controller, "SyncToProgramScene", Qt::DirectConnection),
				      "same-Main reconciliation is callable without switching scenes");
				check(replacementUuid == obs_source_get_uuid(obs_scene_get_source(portrait())),
				      "reconciling the valid replacement again does not create another root");
			}
			saved["portrait_uuid"] = obs_source_get_uuid(obs_scene_get_source(portrait()));
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
		"Native Qt/libobs local color scenes, actual canvas texture samples, independent Cut/Fade, basic Follow Main, interruption, distinct Studio Mode preview/program, automatic removed-root recovery while idle with a fresh default mirror and restart persistence. No devices, active-output recovery, deleted-layout reconstruction, quick/override/manual parity or native Windows execution."}});
}
void event(obs_frontend_event e, void *)
{
	if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING && !started) {
		if (!startTimer) {
			startTimer = new QTimer(qApp);
			startTimer->setSingleShot(true);
			QObject::connect(startTimer, &QTimer::timeout, qApp, run);
		}
		startTimer->start(1800);
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_PORTRAIT_TRANSITIONS_ROOT");
	restore = qEnvironmentVariableIsSet("OBS_STUDIO_PORTRAIT_TRANSITIONS_RESTORE");
	if (root.isEmpty()) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
	delete startTimer;
	startTimer = nullptr;
}
