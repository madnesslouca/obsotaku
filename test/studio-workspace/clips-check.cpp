/* Opt-in integration checks against actual Qt controls, libobs media and the
 * saved scene collection. No actions run without OBS_STUDIO_CLIP_TEST_REPORT. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QScrollArea>
#include <QTimer>
#include <functional>
#include <stdexcept>

OBS_DECLARE_MODULE()
static QString reportPath, fixture, replacement, replayUUID, originalSceneUUID, unmanagedUUID;
static QMainWindow *window;
static QJsonArray checks;
static bool done = false, ownsRecording = false, ownsReplay = false;
static int64_t pausedTime = 0;
static int wraps = 0;
static int64_t lastTime = 0, expectedA = 0, expectedB = 0;
static QTimer *observer;

static void check(bool condition, const char *description)
{
    checks.append(QJsonObject{{"check", description}, {"passed", condition}});
    if (!condition) throw std::runtime_error(description);
}
static void finish(const QString &error = {})
{
    if (done) return;
    done = true;
    if (observer) observer->stop();
    if (ownsRecording && obs_frontend_recording_active()) obs_frontend_recording_stop();
    if (ownsReplay && obs_frontend_replay_buffer_active()) obs_frontend_replay_buffer_stop();
    OBSSourceAutoRelease unmanaged = obs_get_source_by_uuid(unmanagedUUID.toUtf8().constData());
    if (unmanaged) obs_source_remove(unmanaged);
    OBSSourceAutoRelease previous = obs_get_source_by_uuid(originalSceneUUID.toUtf8().constData());
    if (previous) obs_frontend_set_current_scene(previous);
    QFile file(reportPath);
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(QJsonObject{{"passed", error.isEmpty()}, {"error", error}, {"checks", checks},
            {"replay_uuid", replayUUID}, {"ab_wraps", wraps}, {"marker_a_ms", double(expectedA)}, {"marker_b_ms", double(expectedB)}}).toJson());
}
static void later(int ms, std::function<void()> callback)
{
    QTimer::singleShot(ms, qApp, [callback]() {
        if (done) return;
        try { callback(); } catch (const std::exception &error) { finish(QString::fromUtf8(error.what())); }
    });
}
template<typename T> static T *child(const char *name)
{
    auto *result = window->findChild<T *>(QString::fromUtf8(name));
    check(result != nullptr, name);
    return result;
}
static OBSSourceAutoRelease replay()
{
    return obs_get_source_by_uuid(replayUUID.toUtf8().constData());
}
static void registerClip(const QString &path)
{
    check(QMetaObject::invokeMethod(child<QDockWidget>("studioClipsDock"), "RegisterClip", Qt::DirectConnection,
          Q_ARG(QString, path)), "register clip through real saved-clip hook");
}
static void seekTo(int64_t ms)
{
    auto source = replay();
    auto *slider = child<QSlider>("studioReplaySeek");
    slider->setValue(ms * 1000 / obs_source_media_get_duration(source));
    QMetaObject::invokeMethod(slider, "sliderReleased", Qt::DirectConnection);
}
static void checkNativeClipHook()
{
    check(!obs_frontend_replay_buffer_active(), "no external replay buffer before save-hook test");
    obs_frontend_replay_buffer_start();
    ownsReplay = true;
    later(3000, []() {
        check(obs_frontend_replay_buffer_active(), "native replay buffer starts for save-hook test");
        obs_frontend_replay_buffer_save();
        later(2000, []() {
            char *last = obs_frontend_get_last_replay();
            const QString path = QString::fromUtf8(last ? last : "");
            bfree(last);
            check(!path.isEmpty() && QFile(path).size() > 0, "native buffer saves new clip");
            auto *library = child<QListWidget>("studioClipLibrary");
            bool found = false;
            for (int i = 0; i < library->count(); ++i)
                found |= library->item(i)->data(Qt::UserRole).toString() == path;
            check(found, "actual replay-saved event populates clip library");
            auto source = replay();
            OBSDataAutoRelease settings = obs_source_get_settings(source);
            check(QString::fromUtf8(obs_data_get_string(settings, "local_file")) == path,
                  "actual replay-saved event retargets following layer");
            obs_frontend_replay_buffer_stop();
            later(1000, []() {
                check(!obs_frontend_replay_buffer_active(), "test replay buffer stops");
                ownsReplay = false;
                child<QDockWidget>("studioClipsDock")->raise();
                window->grab().save(reportPath + ".png");
                finish();
            });
        });
    });
}
static void checkFollow()
{
    auto source = replay();
    obs_source_media_play_pause(source, true);
    auto *follow = child<QCheckBox>("studioReplayFollow");
    check(follow->isChecked(), "replay follows saved clips by default");
    registerClip(replacement);
    later(1000, []() {
        auto source = replay();
        OBSDataAutoRelease settings = obs_source_get_settings(source);
        OBSDataAutoRelease metadata = obs_source_get_private_settings(source);
        check(QString::fromUtf8(obs_data_get_string(settings, "local_file")) == replacement, "following replay loads newly saved clip");
        check(obs_data_get_int(metadata, "studio_replay_a") == 0 && obs_data_get_int(metadata, "studio_replay_b") == 0,
              "new clip resets A/B marks");
        OBSSourceAutoRelease unmanaged = obs_get_source_by_uuid(unmanagedUUID.toUtf8().constData());
        OBSDataAutoRelease other = obs_source_get_settings(unmanaged);
        check(QString::fromUtf8(obs_data_get_string(other, "local_file")) == fixture, "ordinary media source is not retargeted");
        child<QCheckBox>("studioReplayFollow")->click();
        registerClip(fixture);
        later(500, []() {
            auto source = replay();
            OBSDataAutoRelease settings = obs_source_get_settings(source);
            check(QString::fromUtf8(obs_data_get_string(settings, "local_file")) == replacement, "fixed replay ignores later clips");
            child<QCheckBox>("studioReplayLoop")->setChecked(false);
            // Exercise the real click handler, restoring follow and loop for persistence.
            child<QCheckBox>("studioReplayLoop")->click();
            child<QCheckBox>("studioReplayFollow")->click();
            registerClip(fixture);
            later(1000, []() {
                auto source = replay();
                obs_source_media_restart(source);
                obs_frontend_recording_start();
                ownsRecording = true;
                later(1500, []() {
                    check(obs_frontend_recording_active(), "replay scene records locally");
                    later(4000, []() {
                        obs_frontend_recording_stop();
                        later(1500, []() {
                            check(!obs_frontend_recording_active(), "replay recording stops");
                            ownsRecording = false;
                            checkNativeClipHook();
                        });
                    });
                });
            });
        });
    });
}
static void checkRange()
{
    auto source = replay();
    obs_source_media_play_pause(source, true);
    seekTo(1000);
    later(500, []() {
        expectedA = obs_source_media_get_time(replay());
        child<QPushButton>("studioReplayMarkA")->click();
        seekTo(6500);
        later(500, []() {
            expectedB = obs_source_media_get_time(replay());
            child<QPushButton>("studioReplayMarkB")->click();
            auto source = replay();
            OBSDataAutoRelease metadata = obs_source_get_private_settings(source);
            check(std::abs(obs_data_get_int(metadata, "studio_replay_a") - expectedA) < 50,
                  "A marker follows seek position");
            check(std::abs(obs_data_get_int(metadata, "studio_replay_b") - expectedB) < 50 && expectedB > expectedA + 100,
                  "B marker follows seek position");
            child<QCheckBox>("studioReplayLoop")->click();
            child<QPushButton>("studioReplayPlay")->click();
            observer = new QTimer(qApp);
            observer->setInterval(100);
            QObject::connect(observer, &QTimer::timeout, qApp, []() {
                auto source = replay();
                int64_t now = obs_source_media_get_time(source);
                if (now + 500 < lastTime) ++wraps;
                lastTime = now;
            });
            observer->start();
            later(10500, []() {
                observer->stop();
                check(wraps >= 2, "A/B loop performs at least two actual seeks");
                auto source = replay();
                check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING, "A/B loop keeps playing");
                checkFollow();
            });
        });
    });
}
static void checkPlayback()
{
    auto source = replay();
    check(obs_source_get_width(source) == 1920 && obs_source_get_height(source) == 1080, "replay decodes clip image at full size");
    check((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0, "replay retains native audio output");
    check(obs_source_media_get_duration(source) > 15000, "clip duration is available");
    check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING, "inserted replay plays");
    child<QPushButton>("studioReplayPlay")->click();
    later(300, []() {
        auto source = replay();
        check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED, "pause button pauses native media");
        pausedTime = obs_source_media_get_time(source);
        later(400, []() {
            auto source = replay();
            check(std::abs(obs_source_media_get_time(source) - pausedTime) < 100, "paused timeline remains stable");
            seekTo(5000);
            later(600, []() {
                auto source = replay();
                check(obs_source_media_get_time(source) >= 2800 && obs_source_media_get_time(source) <= 5200, "timeline seeks to native preceding keyframe");
                auto *speed = child<QDoubleSpinBox>("studioReplaySpeed");
                speed->setValue(0.5);
                QMetaObject::invokeMethod(speed, "editingFinished", Qt::DirectConnection);
                later(700, []() {
                    auto source = replay();
                    OBSDataAutoRelease settings = obs_source_get_settings(source);
                    check(obs_data_get_int(settings, "speed_percent") == 50, "speed control changes FFmpeg playback speed");
                    auto *speed = child<QDoubleSpinBox>("studioReplaySpeed");
                    speed->setValue(1.0);
                    QMetaObject::invokeMethod(speed, "editingFinished", Qt::DirectConnection);
                    later(700, checkRange);
                });
            });
        });
    });
}
static void runCheck()
{
    window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    check(window != nullptr, "main window");
    check(!obs_frontend_recording_active() && !obs_frontend_streaming_active(), "no active outputs before test");
    child<QAction>("studioClipLibraryAction")->trigger();
    check(child<QDockWidget>("studioClipsDock")->isVisible(), "clip library opens from Clip menu");
    if (qEnvironmentVariableIsSet("OBS_STUDIO_CLIP_PERSISTENCE_ONLY")) {
        auto *library = child<QListWidget>("studioClipLibrary");
        check(library->count() >= 2, "library entries survive a process restart");
        window->showNormal();
        window->resize(1650, 950);
        later(800, []() {
            auto *layers = child<QListWidget>("studioReplayLayers");
            check(layers->count() >= 1, "managed replay source survives restart");
            replayUUID = layers->item(0)->data(Qt::UserRole).toString();
            auto source = replay();
            OBSDataAutoRelease metadata = obs_source_get_private_settings(source);
            check(obs_data_get_bool(metadata, "studio_replay_follow"), "follow option persists in scene collection");
            check(obs_data_get_bool(metadata, "studio_replay_loop"), "loop option persists in scene collection");
            check(window->width() <= 1650 && window->height() <= 950, "library fits a 1650 by 950 window");
            auto *scroll = child<QScrollArea>("studioClipsScroll");
            auto *mark = child<QPushButton>("studioReplayMarkB");
            scroll->ensureWidgetVisible(mark);
            check(scroll->viewport()->rect().contains(mark->mapTo(scroll->viewport(), mark->rect().center())),
                  "playback controls remain reachable through scrolling");
            window->grab().save(reportPath + ".png");
            finish();
        });
        return;
    }
    fixture = qEnvironmentVariable("OBS_STUDIO_CLIP_FIXTURE");
    replacement = qEnvironmentVariable("OBS_STUDIO_CLIP_REPLACEMENT");
    check(QFile::exists(fixture) && QFile::exists(replacement) && fixture != replacement, "two local clip fixtures exist");
    OBSSourceAutoRelease previous = obs_frontend_get_current_scene();
    originalSceneUUID = QString::fromUtf8(obs_source_get_uuid(previous));
    OBSSceneAutoRelease scene = obs_scene_create("Studio Linux — replay de clips");
    obs_frontend_set_current_scene(obs_scene_get_source(scene));
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_set_string(settings, "local_file", fixture.toUtf8().constData());
    OBSSourceAutoRelease other = obs_source_create("ffmpeg_source", "Studio clip test — ordinary media", settings, nullptr);
    unmanagedUUID = QString::fromUtf8(obs_source_get_uuid(other));
    obs_sceneitem_set_visible(obs_scene_add(scene, other), false);
    registerClip(fixture);
    int count = child<QListWidget>("studioClipLibrary")->count();
    registerClip(fixture);
    check(child<QListWidget>("studioClipLibrary")->count() == count, "same clip is deduplicated");
    registerClip(QStringLiteral("/nonexistent/clip.mkv"));
    check(child<QListWidget>("studioClipLibrary")->count() == count, "missing clip is rejected");
    later(600, []() {
        child<QPushButton>("studioClipInsert")->click();
        later(1200, []() {
            auto *layers = child<QListWidget>("studioReplayLayers");
            check(layers->count() == 1, "insert creates one managed replay layer");
            replayUUID = layers->item(0)->data(Qt::UserRole).toString();
            child<QAction>("actionMainUndo")->trigger();
            later(400, []() {
                check(!replay() || obs_source_removed(replay()), "undo removes the newly added replay");
                child<QAction>("actionMainRedo")->trigger();
                later(1200, []() {
                    check(bool(replay()), "redo restores the replay with its UUID");
                    auto source = replay();
                    OBSDataAutoRelease metadata = obs_source_get_private_settings(source);
                    check(obs_data_get_bool(metadata, "studio_replay_managed"), "redo restores replay metadata");
                    checkPlayback();
                });
            });
        });
    });
}
static void event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) later(1500, runCheck);
}
bool obs_module_load(void)
{
    reportPath = qEnvironmentVariable("OBS_STUDIO_CLIP_TEST_REPORT");
    if (reportPath.isEmpty()) return false;
    obs_frontend_add_event_callback(event, nullptr);
    return true;
}
void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(event, nullptr);
}
