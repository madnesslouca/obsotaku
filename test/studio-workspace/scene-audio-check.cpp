/* Opt-in regression against production UI/model; only our disposable scenes. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QAction>
#include <QApplication>
#include <QAbstractSlider>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QTest>
#include <QTimer>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <vector>

OBS_DECLARE_MODULE()
static QMainWindow *window;
static QString reportPath, originalUUID;
static QJsonArray checks;
static OBSSourceAutoRelease source, sceneA, sceneB;
static bool finished = false, originalStudioMode = false, keepFixture = false;
static std::vector<std::function<void()>> steps;
static size_t stepIndex = 0;
static double changedGain;
static void check(bool ok, const char *label)
{
    checks.append(QJsonObject{{"check", label}, {"passed", ok}});
    if (!ok) throw std::runtime_error(label);
}
static void finish(const QString &error = {})
{
    if (finished) return;
    finished = true;
    if (originalStudioMode != obs_frontend_preview_program_mode_active())
        obs_frontend_set_preview_program_mode(originalStudioMode);
    OBSSourceAutoRelease original = obs_get_source_by_uuid(originalUUID.toUtf8().constData());
    if (original) obs_frontend_set_current_scene(original);
    if (!keepFixture || !error.isEmpty()) {
        if (sceneA) obs_source_remove(sceneA);
        if (sceneB) obs_source_remove(sceneB);
        if (source) obs_source_remove(source);
    }
    obs_frontend_save();
    QFile file(reportPath);
    if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
        {"passed", error.isEmpty()}, {"error", error}, {"checks", checks},
        {"original_scene_uuid", originalUUID}, {"fixtures_kept_for_restart", keepFixture && error.isEmpty()},
        {"scope", "production Qt controls and native scene changes; disposable audio source/scenes; no stream"}
    }).toJson());
    source = nullptr; sceneA = nullptr; sceneB = nullptr;
}
static void next()
{
    QTimer::singleShot(650, qApp, []() {
        if (finished) return;
        try {
            if (stepIndex == steps.size()) { finish(); return; }
            steps[stepIndex++](); next();
        } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); }
    });
}
template<typename T> static T *child(const char *name)
{
    auto *widget = window->findChild<T *>(QString::fromUtf8(name));
    check(widget != nullptr, name); return widget;
}
static void click(QWidget *widget)
{
    check(widget != nullptr, "pointer target exists");
    for (auto *p = widget->parentWidget(); p; p = p->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(p)) scroll->ensureWidgetVisible(widget);
    check(widget->isVisible() && widget->isEnabled(), "pointer target available");
    QPoint pos = widget->rect().center();
    if (qobject_cast<QCheckBox *>(widget)) pos.setX(8);
    QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, pos);
}
static void openAudio()
{
    const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
    check(QMetaObject::invokeMethod(window, "OpenStudioAudioRouting", Qt::DirectConnection, Q_ARG(QString, uuid)), "open selected audio source");
}
static void setGain(double db)
{
    auto *gain = child<QDoubleSpinBox>("studioAudioGain");
    gain->setValue(db);
    check(QMetaObject::invokeMethod(gain, "editingFinished", Qt::DirectConnection), "commit gain");
}
static void volume(double db, const char *label)
{
    check(std::abs(obs_mul_to_db(obs_source_get_volume(source)) - db) < .15, label);
}
static QWidget *strip()
{
    for (auto *widget : window->findChildren<QWidget *>())
        if (QByteArray(widget->metaObject()->className()) == "VolumeControl" && widget->objectName() == "Studio Mixer QA — shared")
            return widget;
    throw std::runtime_error("native mixer strip not found");
}
static void run()
{
    window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    check(!obs_frontend_streaming_active() && !obs_frontend_recording_active(), "outputs idle");
    originalStudioMode = obs_frontend_preview_program_mode_active();
    OBSSourceAutoRelease original = obs_frontend_get_current_scene();
    originalUUID = QString::fromUtf8(obs_source_get_uuid(original));
    if (originalStudioMode) obs_frontend_set_preview_program_mode(false);
    const bool persistence = qEnvironmentVariableIsSet("OBS_STUDIO_SCENE_AUDIO_PERSISTENCE");
    keepFixture = !persistence;
    if (persistence) {
        source = obs_get_source_by_name("Studio Mixer QA — shared");
        sceneA = obs_get_source_by_name("Studio Mixer QA — A");
        sceneB = obs_get_source_by_name("Studio Mixer QA — B");
        check(source && sceneA && sceneB, "persisted fixtures restored");
        steps = {
            [] { obs_frontend_set_current_scene(sceneA); },
            [] { openAudio(); volume(-12, "scene A gain persists after restart"); check(obs_source_muted(source), "scene A mute persists after restart"); check(!child<QCheckBox>("studioAudioSceneLink")->isChecked(), "scene A remains unlinked"); obs_frontend_set_current_scene(sceneB); },
            [] { openAudio(); volume(-6, "global gain persists after restart"); check(!obs_source_muted(source), "global mute persists after restart"); check(child<QCheckBox>("studioAudioSceneLink")->isChecked(), "scene B remains globally linked"); }
        };
    } else {
        OBSSourceAutoRelease prior = obs_get_source_by_name("Studio Mixer QA — shared");
        check(!prior, "fixture name unused");
        OBSDataAutoRelease settings = obs_data_create();
        const QByteArray fixturePath = qgetenv("OBS_STUDIO_SCENE_AUDIO_FIXTURE");
        if (!fixturePath.isEmpty()) { obs_data_set_string(settings, "local_file", fixturePath.constData()); obs_data_set_bool(settings, "looping", true); }
        source = obs_source_create("ffmpeg_source", "Studio Mixer QA — shared", settings, nullptr);
        check(source != nullptr, "disposable audio source created");
        OBSSceneAutoRelease a = obs_scene_create("Studio Mixer QA — A");
        OBSSceneAutoRelease b = obs_scene_create("Studio Mixer QA — B");
        sceneA = obs_source_get_ref(obs_scene_get_source(a));
        sceneB = obs_source_get_ref(obs_scene_get_source(b));
        obs_scene_add(a, source); obs_scene_add(b, source);
        obs_source_set_volume(source, float(obs_db_to_mul(-6)));
        steps = {
            [] { obs_frontend_set_current_scene(sceneA); },
            [] { openAudio(); volume(-6, "new scene uses global gain"); check(child<QCheckBox>("studioAudioSceneLink")->isChecked(), "default is global link"); click(child<QCheckBox>("studioAudioSceneLink")); setGain(-12); click(child<QCheckBox>("studioAudioMute")); },
            [] { volume(-12, "unlinked scene gain edited"); check(obs_source_muted(source), "unlinked scene mute edited");
                 auto *slider = strip()->findChild<QAbstractSlider *>(); check(slider, "native channel fader exists");
                 slider->setFocus(); QTest::keyClick(slider, Qt::Key_PageUp); changedGain = obs_mul_to_db(obs_source_get_volume(source));
                 check(std::abs(changedGain + 12) > .01, "keyboard moves native channel fader"); obs_frontend_set_current_scene(sceneB); },
            [] { volume(-6, "native fader edit stays in scene A"); child<QAction>("actionMainUndo")->trigger(); volume(-6, "undo native fader leaves scene B unchanged"); obs_frontend_set_current_scene(sceneA); },
            [] { volume(-12, "undo native fader restores scene A"); child<QAction>("actionMainRedo")->trigger(); volume(changedGain, "redo native fader restores scene A edit"); child<QAction>("actionMainUndo")->trigger();
                 click(strip()->findChild<QPushButton *>("studioMixerMute")); check(!obs_source_muted(source), "native channel mute changes local scene"); obs_frontend_set_current_scene(sceneB); },
            [] { check(!obs_source_muted(source), "scene B remains globally unmuted"); child<QAction>("actionMainUndo")->trigger(); check(!obs_source_muted(source), "undo native mute leaves scene B unchanged"); obs_frontend_set_current_scene(sceneA); },
            [] { volume(-12, "native fader undo retained"); check(obs_source_muted(source), "native mute undo restores original scene"); obs_frontend_set_current_scene(sceneB); },
            [] { openAudio(); volume(-6, "other scene restores global gain"); check(!obs_source_muted(source), "other scene restores global mute"); check(child<QCheckBox>("studioAudioSceneLink")->isChecked(), "other scene remains linked"); setGain(-9); obs_frontend_set_current_scene(sceneA); },
            [] { openAudio(); volume(-12, "return restores local gain"); check(obs_source_muted(source), "return restores local mute"); click(child<QCheckBox>("studioAudioSceneLink")); volume(-9, "relink restores updated global gain"); check(!obs_source_muted(source), "relink restores updated global mute"); child<QAction>("actionMainUndo")->trigger(); },
            [] { volume(-12, "undo relink restores local gain"); check(obs_source_muted(source), "undo relink restores local mute"); check(!child<QCheckBox>("studioAudioSceneLink")->isChecked(), "undo relink restores unlink"); child<QAction>("actionMainRedo")->trigger(); },
            [] { volume(-9, "redo relink restores global gain"); click(child<QCheckBox>("studioAudioSceneLink")); setGain(-18); },
            [] { obs_frontend_set_current_scene(sceneB); },
            [] { volume(-9, "global unaffected by local edit"); child<QAction>("actionMainUndo")->trigger(); volume(-9, "undo local gain while elsewhere preserves program"); obs_frontend_set_current_scene(sceneA); },
            [] { volume(-9, "undo changed the original local scene"); openAudio(); setGain(-12); click(child<QCheckBox>("studioAudioMute")); obs_frontend_set_current_scene(sceneB); },
            [] { volume(-9, "global remains unchanged"); obs_frontend_set_preview_program_mode(true); },
            [] { obs_frontend_set_current_preview_scene(sceneA); },
            [] { volume(-9, "preview selection does not apply local gain"); check(!obs_source_muted(source), "preview selection does not apply local mute"); obs_frontend_set_preview_program_mode(false); obs_frontend_set_current_scene(sceneB); },
            [] { openAudio(); setGain(-6); obs_frontend_set_current_scene(sceneA); },
            [] { openAudio(); volume(-12, "scene A final gain"); check(obs_source_muted(source), "scene A final mute"); click(child<QPushButton>("studioAudioSourceProperties")); },
            [] { bool found = false; for (auto *widget : QApplication::topLevelWidgets()) if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == "OBSBasicProperties") { found = true; if (auto *buttons = widget->findChild<QDialogButtonBox *>()) { if (auto *cancel = buttons->button(QDialogButtonBox::Cancel)) cancel->click(); } break; } check(found, "source properties opens native device editor"); },
            [] { auto *edit = strip()->findChild<QPushButton *>("studioMixerEdit"); check(edit, "EDIT available on native strip"); click(edit); },
            [] { check(child<QDockWidget>("studioAudioDock")->isVisible(), "strip EDIT opens audio panel"); auto *link = strip()->findChild<QPushButton *>("studioMixerLink"); check(link && !link->isChecked(), "strip reflects scene-local state"); click(link); },
            [] { volume(-6, "strip link restores global gain"); check(!obs_source_muted(source), "strip link restores global mute"); auto *link = strip()->findChild<QPushButton *>("studioMixerLink"); click(link); setGain(-12); click(child<QCheckBox>("studioAudioMute")); },
            [] { child<QDockWidget>("studioAudioDock")->grab().save(reportPath + ".png"); click(child<QPushButton>("studioAudioAddSource")); },
            [] { bool found = false; for (auto *widget : QApplication::topLevelWidgets()) if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == "OBSBasicSourceSelect") { found = true; widget->close(); break; } check(found, "add source opens native platform source selector"); obs_frontend_save(); }
        };
    }
    next();
}
static void event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
        QTimer::singleShot(1800, qApp, [] { try { run(); } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); } });
}
bool obs_module_load()
{
    reportPath = qEnvironmentVariable("OBS_STUDIO_SCENE_AUDIO_REPORT");
    if (reportPath.isEmpty()) return false;
    obs_frontend_add_event_callback(event, nullptr); return true;
}
void obs_module_unload() { obs_frontend_remove_event_callback(event, nullptr); source = nullptr; sceneA = nullptr; sceneB = nullptr; }
