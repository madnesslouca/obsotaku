/* Opt-in integration probe: actual UI controls, browser rendering, multitrack
 * recording and production MultiStreamManager sending ONLY to loopback sinks.
 * Credential-store writes are deliberately excluded from this backend test. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamAudioRouting.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <functional>
#include <cmath>
#include <memory>
#include <stdexcept>

OBS_DECLARE_MODULE()
// The probe never toggles persisted channels; production Start/Stop are intact.
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
    error = "Credential persistence is outside the local backend probe";
    return false;
}
static QString reportPath, root, previousScene, toneA, toneB, browserFixture;
static QMainWindow *window;
static QJsonArray checks;
static bool done = false;
static std::unique_ptr<MultiStreamManager> manager;
static OBSEncoderAutoRelease video, audioA, audioB, audioC;
static OBSOutputAutoRelease recording, primaryOutput;
static OBSServiceAutoRelease primaryService;
static QStringList widgets;

static void check(bool condition, const char *name)
{
    checks.append(QJsonObject{{"check", name}, {"passed", condition}});
    if (!condition) throw std::runtime_error(name);
}
static void finish(const QString &error = {})
{
    if (done) return;
    done = true;
    if (manager) manager->Stop(true);
    if (recording && obs_output_active(recording)) obs_output_force_stop(recording);
    if (primaryOutput && obs_output_active(primaryOutput)) obs_output_force_stop(primaryOutput);
    QTimer::singleShot(1200, qApp, [error]() {
        manager.reset(); recording = nullptr; primaryOutput = nullptr; primaryService = nullptr; video = nullptr; audioA = nullptr; audioB = nullptr; audioC = nullptr;
        for (const QString &uuid : {toneA, toneB, browserFixture}) {
            OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
            if (source) obs_source_remove(source);
        }
        OBSSourceAutoRelease previous = obs_get_source_by_uuid(previousScene.toUtf8().constData());
        if (previous) obs_frontend_set_current_scene(previous);
        QFile file(reportPath);
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
            {"passed", error.isEmpty()}, {"error", error}, {"checks", checks},
            {"scope", "production manager Start/Stop and real loopback RTMP delivery; excludes account storage/OAuth and public platforms"}
        }).toJson());
    });
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
    check(result != nullptr, name); return result;
}
static OBSSourceAutoRelease source(const QString &uuid) { return obs_get_source_by_uuid(uuid.toUtf8().constData()); }
static void undo() { child<QAction>("actionMainUndo")->trigger(); }
static void selectTone()
{
    auto *table = child<QTableWidget>("studioAudioRouting");
    int row = -1;
    for (int i = 0; i < table->rowCount(); ++i) if (table->item(i, 0)->data(Qt::UserRole).toString() == toneA) row = i;
    check(row >= 0, "audio matrix includes test source");
    table->setCurrentCell(row, 0);
}
static void startOutputs()
{
    OBSDataAutoRelease v = obs_data_create();
    obs_data_set_string(v, "rate_control", "CBR"); obs_data_set_int(v, "bitrate", 2500);
    obs_data_set_int(v, "keyint_sec", 2); obs_data_set_string(v, "preset", "p4");
    video = obs_video_encoder_create("obs_nvenc_h264_tex", "studio_local_multistream_video", v, nullptr);
    check(bool(video), "native NVENC video encoder created"); obs_encoder_set_video(video, obs_get_video());
    OBSDataAutoRelease a = obs_data_create(); obs_data_set_int(a, "bitrate", 160);
    audioA = obs_audio_encoder_create("ffmpeg_aac", "studio_local_track_1", a, 0, nullptr);
    audioB = obs_audio_encoder_create("ffmpeg_aac", "studio_local_track_2", a, 1, nullptr);
    audioC = obs_audio_encoder_create("ffmpeg_aac", "studio_local_browser_track_3", a, 2, nullptr);
    check(bool(audioA) && bool(audioB) && bool(audioC), "three native audio encoders created with different mix indices");
    obs_encoder_set_audio(audioA, obs_get_audio()); obs_encoder_set_audio(audioB, obs_get_audio()); obs_encoder_set_audio(audioC, obs_get_audio());
    manager = std::make_unique<MultiStreamManager>();
    MultiStreamChannel first; first.id = "studio-local-a"; first.displayName = "Local A — track 1";
    first.server = "rtmp://127.0.0.1:19351/live"; first.streamKey = "studio-a"; first.audioMixIndex = 0;
    MultiStreamChannel second = first; second.id = "studio-local-b"; second.displayName = "Local B — track 2";
    second.server = "rtmp://127.0.0.1:19352/live"; second.streamKey = "studio-b"; second.audioMixIndex = 1;
    std::string error;
    check(manager->Configure({first, second}, error), "production manager accepts two local destinations");
    MultiStreamOutputSettings settings; settings.reconnect = {0, 1}; settings.bindIp = "127.0.0.1";
    check(!manager->Start(video, audioA, settings, error), "missing selected audio track fails instead of sending another mix");
    check(error.find("Audio track 2") != std::string::npos, "missing-track error identifies requested track");
    bool noOutput = true;
    for (auto &entry : manager->ChannelOutputs()) noOutput &= !entry.output;
    check(noOutput && !manager->IsActive(), "missing-track validation starts no destination");
    first.enabled = false; second.enabled = false;
    check(manager->Configure({first, second}, error), "disabled destinations configure without credentials changes");
    check(manager->Start(video, audioA, settings, error), "disabled destinations prepare without sending");
    check(!manager->SetChannelEnabled(second.id, true, error) && error.find("Audio track 2") != std::string::npos,
          "enabling a prepared destination cannot silently substitute its audio mix");
    for (auto &entry : manager->ChannelOutputs()) check(!obs_output_active(entry.output), "disabled validation sends no packets");
    manager->Stop(true); first.enabled = true; second.enabled = true;
    check(manager->Configure({first, second}, error), "manager can configure again after preflight failure");
    MultiStreamChannel primary = first; primary.id = "studio-local-primary"; primary.displayName = "Local primary — track 3";
    primary.server = "rtmp://127.0.0.1:19354/live"; primary.streamKey = "studio-primary"; primary.audioMixIndex = 2;
    check(manager->Configure({first, second, primary}, error), "primary plus two additional destinations configure");
    manager->SetPrimaryChannelId(primary.id);
    OBSDataAutoRelease serviceSettings = obs_data_create();
    obs_data_set_string(serviceSettings, "server", primary.server.c_str()); obs_data_set_string(serviceSettings, "key", primary.streamKey.c_str());
    primaryService = obs_service_create("rtmp_custom", "studio_local_primary_service", serviceSettings, nullptr);
    primaryOutput = obs_output_create("rtmp_output", "studio_local_primary_output", nullptr, nullptr);
    check(bool(primaryService) && bool(primaryOutput), "promoted-primary test output created");
    obs_output_set_service(primaryOutput, primaryService); obs_output_set_video_encoder(primaryOutput, video);
    obs_output_set_audio_encoder(primaryOutput, audioA, 0); obs_output_set_audio_encoder(primaryOutput, audioB, 1);
    check(!PrepareMultistreamPrimaryAudio(*manager, primaryOutput, {audioA.Get(), audioB.Get()}, error),
          "primary refuses unavailable selected track");
    check(obs_output_get_audio_encoder(primaryOutput, 0) == audioA.Get(), "failed primary validation leaves audio binding unchanged");
    check(PrepareMultistreamPrimaryAudio(*manager, primaryOutput, {audioA.Get(), audioB.Get(), audioC.Get()}, error),
          "primary helper selects requested track 3");
    check(obs_encoder_get_mixer_index(obs_output_get_audio_encoder(primaryOutput, 0)) == 2, "primary uses browser mix instead of default mix");
    check(obs_output_get_audio_encoder(primaryOutput, 1) == nullptr, "custom primary clears stale unsupported VOD encoder");
    check(obs_output_start(primaryOutput), "primary sends to third local RTMP sink");
    check(manager->Start(video, audioA, {audioA.Get(), audioB.Get(), audioC.Get()}, settings, error), "two RTMP destinations start through production manager");
    OBSDataAutoRelease rec = obs_data_create();
    obs_data_set_string(rec, "path", (root + "/audio-multitrack.mkv").toUtf8().constData());
    recording = obs_output_create("ffmpeg_muxer", "studio_local_multitrack", rec, nullptr);
    check(bool(recording), "native multitrack file output created");
    obs_output_set_video_encoder(recording, video);
    obs_output_set_audio_encoder(recording, audioA, 0); obs_output_set_audio_encoder(recording, audioB, 1); obs_output_set_audio_encoder(recording, audioC, 2);
    check(obs_output_start(recording), "recording starts with three independent audio tracks");
    later(4000, []() {
        auto outputs = manager->ChannelOutputs();
        check(outputs.size() == 2, "two destination outputs exist simultaneously");
        for (auto &entry : outputs) {
            check(obs_output_active(entry.output), "local RTMP destination is active");
            check(obs_output_get_total_bytes(entry.output) > 20000, "local destination receives encoded bytes");
            check(obs_output_get_video_encoder(entry.output) == video.Get(), "destinations share main video encoder");
            const size_t expected = entry.id == "studio-local-a" ? 0 : 1;
            check(obs_encoder_get_mixer_index(obs_output_get_audio_encoder(entry.output, 0)) == expected,
                  "destination uses its explicitly selected audio mix");
        }
        check(obs_output_active(primaryOutput) && obs_output_get_total_bytes(primaryOutput) > 20000, "primary delivers its selected audio/video concurrently");
        auto snapshots = manager->Snapshot();
        check(snapshots.size() == 2, "both channel health snapshots available");
        for (const auto &snapshot : snapshots) check(snapshot.state == MultiStreamChannelState::Live, "destination reaches Live state");
        later(7000, []() {
            manager->Stop(false); obs_output_stop(recording); obs_output_stop(primaryOutput);
            later(2000, []() {
                check(!manager->IsActive(), "multistream manager stops");
                check(!obs_output_active(primaryOutput), "primary output stops");
                check(!obs_output_active(recording), "multitrack recording stops");
                check(QFile(root + "/audio-multitrack.mkv").size() > 0, "multitrack recording file exists");
                finish();
            });
        });
    });
}
static void prepareDelivery()
{
    auto a = source(toneA), b = source(toneB);
    obs_source_set_audio_mixers(a, 1); obs_source_set_audio_mixers(b, 2);
    obs_source_set_volume(a, 1); obs_source_set_volume(b, 1);
    obs_source_set_monitoring_type(a, OBS_MONITORING_TYPE_NONE);
    obs_source_set_monitoring_type(b, OBS_MONITORING_TYPE_NONE);
    OBSSourceAutoRelease sceneSource = obs_frontend_get_current_scene();
    obs_scene_t *scene = obs_scene_from_source(sceneSource);
    obs_sceneitem_set_visible(obs_scene_find_source(scene, "Studio test — tone A"), true);
    obs_sceneitem_set_visible(obs_scene_find_source(scene, "Studio test — tone B"), true);
    later(1200, startOutputs);
}
static void testAudio()
{
    child<QPushButton>("studioAudioRoutingButton")->click();
    check(child<QDockWidget>("studioAudioDock")->isVisible(), "audio routing panel opens");
    later(500, []() {
        selectTone();
        auto a = source(toneA);
        auto *table = child<QTableWidget>("studioAudioRouting");
        table->item(table->currentRow(), 2)->setCheckState(Qt::Checked);
        check(obs_source_get_audio_mixers(a) == 3, "routing checkbox adds source to track 2");
        undo(); check(obs_source_get_audio_mixers(a) == 1, "routing supports native Undo");
        auto *gain = child<QDoubleSpinBox>("studioAudioGain"); gain->setValue(-6);
        QMetaObject::invokeMethod(gain, "editingFinished", Qt::DirectConnection);
        check(std::abs(obs_source_get_volume(a) - .501187) < .001, "gain changes actual source amplitude");
        undo(); check(std::abs(obs_source_get_volume(a) - 1) < .001, "gain supports Undo");
        child<QCheckBox>("studioAudioMute")->click(); check(obs_source_muted(a), "Mute changes native source");
        undo(); check(!obs_source_muted(a), "Mute supports Undo");
        child<QCheckBox>("studioAudioMono")->click(); check(obs_source_get_flags(a) & OBS_SOURCE_FLAG_FORCE_MONO, "mono changes native downmix flag");
        undo();
        auto *balance = child<QDoubleSpinBox>("studioAudioBalance"); balance->setValue(.25);
        QMetaObject::invokeMethod(balance, "editingFinished", Qt::DirectConnection);
        check(std::abs(obs_source_get_balance_value(a) - .25) < .001, "balance controls native panning"); undo();
        auto *sync = child<QSpinBox>("studioAudioSync"); sync->setValue(123);
        QMetaObject::invokeMethod(sync, "editingFinished", Qt::DirectConnection);
        check(obs_source_get_sync_offset(a) == 123000000, "audio delay uses milliseconds correctly"); undo();
        auto *monitor = child<QComboBox>("studioAudioMonitor");
        monitor->setCurrentIndex(monitor->findData(int(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT)));
        QMetaObject::invokeMethod(monitor, "activated", Qt::DirectConnection, Q_ARG(int, monitor->currentIndex()));
        check(obs_source_get_monitoring_type(a) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT, "Cue controls native monitoring mode");
        undo(); check(obs_source_get_monitoring_type(a) == OBS_MONITORING_TYPE_NONE, "Cue supports Undo");
        child<QPushButton>("studioAudioVoicePreset")->click();
        check(obs_source_filter_count(a) == 3, "voice chain adds three real native filters");
        OBSSourceAutoRelease noise = obs_source_get_filter_by_name(a, "Studio — RNNoise");
        OBSDataAutoRelease noiseSettings = obs_source_get_settings(noise);
        check(!strcmp(obs_data_get_string(noiseSettings, "method"), "rnnoise"), "noise reduction uses RNNoise");
        child<QPushButton>("studioAudioVoicePreset")->click(); check(obs_source_filter_count(a) == 3, "voice chain does not duplicate filters");
        undo(); check(obs_source_filter_count(a) == 0, "voice chain Undo removes only added filters");
        child<QAction>("actionMainRedo")->trigger(); check(obs_source_filter_count(a) == 3, "voice chain Redo restores filters");
        undo(); check(obs_source_filter_count(a) == 0, "test restores tone without effects for spectral validation");
        obs_source_set_volume(a, .25f);
        later(500, []() {
            check(std::abs(child<QDoubleSpinBox>("studioAudioGain")->value() + 12.0) < .15, "panel observes external audio changes");
            obs_source_set_volume(source(toneA), 1);
            prepareDelivery();
        });
    });
}
static void createWidget(int index)
{
    if (index == 3) { testAudio(); return; }
    // A queued dialog interaction runs inside QDialog::exec's event loop.
    later(120, [index]() {
        auto *dialog = child<QDialog>("studioWidgetEditor");
        child<QComboBox>("studioWidgetType")->setCurrentIndex(index);
        child<QLineEdit>("studioWidgetTitle")->setText(index == 2 ? QStringLiteral("Studio Linux — texto local") : QStringLiteral("Widget local"));
        child<QLineEdit>("studioWidgetSubtitle")->setText(QStringLiteral("Sem conta e sem servidor externo"));
        child<QSpinBox>("studioWidgetSeconds")->setValue(90);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    child<QAction>("studioWidgetAddAction")->trigger();
    later(700, [index]() {
        obs_enum_sources([](void *, obs_source_t *source) {
            OBSDataAutoRelease settings = obs_source_get_settings(source);
            if (obs_data_get_bool(settings, "studio_widget")) {
                QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
                if (!widgets.contains(uuid)) widgets.append(uuid);
            }
            return true;
        }, nullptr);
        check(widgets.size() == index + 1, "widget dialog creates native browser source");
        auto widget = source(widgets.last());
        OBSDataAutoRelease settings = obs_source_get_settings(widget);
        check(obs_data_get_bool(settings, "reroute_audio"), "widget browser audio is routed through OBS");
        check(QString::fromUtf8(obs_data_get_string(settings, "url")).startsWith("file:///"), "widget loads local HTML");
        OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
        auto *item = obs_scene_find_source(obs_scene_from_source(scene), obs_source_get_name(widget));
        vec2 pos{60.f, float(60 + index * 270)}; obs_sceneitem_set_pos(item, &pos);
        createWidget(index + 1);
    });
}
static void persistenceCheck()
{
    OBSSourceAutoRelease previous = obs_frontend_get_current_scene();
    previousScene = QString::fromUtf8(obs_source_get_uuid(previous));
    OBSSourceAutoRelease clock = obs_get_source_by_name("Widget — Relógio");
    check(bool(clock), "clock widget survives restart");
    OBSDataAutoRelease settings = obs_source_get_settings(clock);
    check(obs_data_get_bool(settings, "studio_widget"), "widget configuration survives restart");
    check(std::abs(obs_source_get_volume(clock) - .501187) < .001, "audio gain survives restart");
    check((obs_source_get_audio_mixers(clock) & 63) == 3, "source routing mask survives restart");
    check(obs_source_get_sync_offset(clock) == 25000000, "audio synchronization survives restart");
    const QString uuid = QString::fromUtf8(obs_source_get_uuid(clock));
    check(QMetaObject::invokeMethod(window, "OpenStudioAudioRouting", Qt::DirectConnection, Q_ARG(QString, uuid)),
          "mixer can open on a specific source");
    window->showNormal(); window->resize(1650, 950);
    later(600, []() {
        check(std::abs(child<QDoubleSpinBox>("studioAudioGain")->value() + 6) < .15, "mixer displays persisted gain");
        check(child<QSpinBox>("studioAudioSync")->value() == 25, "mixer displays persisted delay");
        check(window->width() <= 1650 && window->height() <= 950, "audio panel fits compact window");
        child<QDockWidget>("studioAudioDock")->grab().save(reportPath + ".png");
        OBSSourceAutoRelease demo = obs_get_source_by_name("Studio Linux — widgets e áudio");
        obs_frontend_set_current_scene(demo);
        later(400, []() {
            auto *list = child<QListView>("sources");
            list->selectionModel()->select(list->model()->index(0, 0), QItemSelectionModel::ClearAndSelect);
            later(200, []() {
                later(100, []() {
                    auto *dialog = child<QDialog>("studioWidgetEditor");
                    child<QLineEdit>("studioWidgetTitle")->setText(QStringLiteral("Widget editado no Linux"));
                    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                });
                child<QAction>("studioWidgetEditAction")->trigger();
                OBSSourceAutoRelease lower = obs_get_source_by_name("Widget — Tarja de texto");
                OBSDataAutoRelease settings = obs_source_get_settings(lower);
                check(QString::fromUtf8(obs_data_get_string(settings, "studio_widget_title")) == "Widget editado no Linux",
                      "existing widget editor changes native browser settings");
                undo();
                OBSDataAutoRelease restored = obs_source_get_settings(lower);
                check(QString::fromUtf8(obs_data_get_string(restored, "studio_widget_title")) == "Studio Linux — texto local",
                      "widget edit supports Undo");
                child<QAction>("actionMainRedo")->trigger();
                OBSDataAutoRelease redone = obs_source_get_settings(lower);
                check(QString::fromUtf8(obs_data_get_string(redone, "studio_widget_title")) == "Widget editado no Linux",
                      "widget edit supports Redo");
                undo();
                OBSSourceAutoRelease clock = obs_get_source_by_name("Widget — Relógio");
                obs_source_set_volume(clock, 1); obs_source_set_audio_mixers(clock, 63); obs_source_set_sync_offset(clock, 0);
                finish();
            });
        });
    });
}
static void runCheck()
{
    window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    check(window != nullptr, "main window");
    check(!obs_frontend_recording_active() && !obs_frontend_streaming_active() && !obs_frontend_replay_buffer_active(), "no active outputs before local test");
    check(obs_source_get_display_name("browser_source") != nullptr, "native browser plugin loaded on Linux");
    if (qEnvironmentVariableIsSet("OBS_STUDIO_FEATURE_PERSISTENCE_ONLY")) { persistenceCheck(); return; }
    OBSSourceAutoRelease previous = obs_frontend_get_current_scene(); previousScene = QString::fromUtf8(obs_source_get_uuid(previous));
    OBSSceneAutoRelease scene = obs_scene_create("Studio Linux — widgets e áudio");
    obs_frontend_set_current_scene(obs_scene_get_source(scene));
    OBSDataAutoRelease background = obs_data_create(); obs_data_set_int(background, "color", 0xff24201c);
    obs_data_set_int(background, "width", 1920); obs_data_set_int(background, "height", 1080);
    OBSSourceAutoRelease color = obs_source_create("color_source_v3", "Studio — fundo widgets", background, nullptr);
    obs_scene_add(scene, color);
    for (int i = 0; i < 2; ++i) {
        OBSDataAutoRelease settings = obs_data_create();
        obs_data_set_string(settings, "local_file", (root + (i ? "/tone-b.wav" : "/tone-a.wav")).toUtf8().constData());
        obs_data_set_bool(settings, "looping", true);
        OBSSourceAutoRelease tone = obs_source_create("ffmpeg_source", i ? "Studio test — tone B" : "Studio test — tone A", settings, nullptr);
        check(bool(tone), "test audio source created");
        obs_source_set_audio_mixers(tone, i ? 2 : 1); obs_source_set_volume(tone, 1);
        obs_sceneitem_set_visible(obs_scene_add(scene, tone), false);
        (i ? toneB : toneA) = QString::fromUtf8(obs_source_get_uuid(tone));
    }
    OBSDataAutoRelease browserSettings = obs_data_create();
    obs_data_set_bool(browserSettings, "is_local_file", false);
    obs_data_set_string(browserSettings, "url", "http://127.0.0.1:19353/browser-fixture.html");
    obs_data_set_bool(browserSettings, "reroute_audio", true);
    obs_data_set_int(browserSettings, "width", 500); obs_data_set_int(browserSettings, "height", 100);
    OBSSourceAutoRelease browser = obs_source_create("browser_source", "Studio test — browser AV", browserSettings, nullptr);
    check(bool(browser), "browser audio/video fixture created");
    obs_source_set_audio_mixers(browser, 4);
    auto *browserItem = obs_scene_add(scene, browser); vec2 browserPos{1300.f, 50.f};
    obs_sceneitem_set_pos(browserItem, &browserPos);
    browserFixture = QString::fromUtf8(obs_source_get_uuid(browser));
    createWidget(0);
}
static void event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) later(1500, runCheck);
}
bool obs_module_load(void)
{
    reportPath = qEnvironmentVariable("OBS_STUDIO_FEATURE_TEST_REPORT");
    root = qEnvironmentVariable("OBS_STUDIO_FEATURE_TEST_ROOT");
    if (reportPath.isEmpty() || root.isEmpty()) return false;
    obs_frontend_add_event_callback(event, nullptr); return true;
}
void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(event, nullptr);
    manager.reset(); recording = nullptr; primaryOutput = nullptr; primaryService = nullptr; video = nullptr; audioA = nullptr; audioB = nullptr; audioC = nullptr;
}
