/* Opt-in portable UI fixture. Device enumeration belongs to a synthetic test input. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QTabBar>
#include <QDial>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <vector>

OBS_DECLARE_MODULE()
static QString reportPath, originalUUID, backendDevice;
static QMainWindow *window;
static QString addedUUID;
static double gainPeakBefore;
static QString extraUUID;
static QPointer<QWidget> channel;
static OBSSourceAutoRelease source, scene, filter;
static QTimer *audioTimer;
static QJsonArray checks;
static QJsonObject facts;
static bool finished = false, originalStudioMode = false;
static std::vector<std::function<void()>> steps;
static size_t stepIndex = 0;
static uint64_t sampleIndex = 0;
static void check(bool ok, const char *label)
{
    checks.append(QJsonObject{{"check", label}, {"passed", ok}});
    if (!ok) throw std::runtime_error(label);
}
static void finish(const QString &error = {})
{
    if (finished) return;
    finished = true;
    if (auto *popup = QApplication::activePopupWidget()) popup->close();
    if (audioTimer) { audioTimer->stop(); audioTimer->deleteLater(); audioTimer = nullptr; }
    OBSSourceAutoRelease original = obs_get_source_by_uuid(originalUUID.toUtf8().constData());
    if (original) obs_frontend_set_current_scene(original);
    if (originalStudioMode) obs_frontend_set_preview_program_mode(true);
    if (scene) obs_source_remove(scene);
    if (source) obs_source_remove(source);
    filter = nullptr; source = nullptr; scene = nullptr;
    obs_frontend_save();
    QTimer::singleShot(900, qApp, []() {
        for (auto *widget : window->findChildren<QWidget *>())
            if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == "VolumeControl" && widget->objectName() == QString::fromUtf8("Studio — microfone"))
                widget->grab().save(reportPath + ".microphone.png");
    });
    QFile file(reportPath);
    if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
        {"passed", error.isEmpty()}, {"error", error}, {"checks", checks}, {"facts", facts},
        {"scope", "actual Qt mouse/keyboard, synthetic audio input/device properties, native filters; no real microphone settings changed, no outputs started"}
    }).toJson());
}
static void later(int ms, std::function<void()> callback)
{
    QTimer::singleShot(ms, qApp, [callback]() { if (finished) return; try { callback(); } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); } });
}
static void next()
{
    later(650, []() { if (stepIndex == steps.size()) { finish(); return; } blog(LOG_INFO, "Studio strip test: step %zu", stepIndex); steps[stepIndex++](); next(); });
}
template<typename T> static T *child(const char *name)
{
    check(bool(channel), "channel remains alive");
    auto *widget = channel->findChild<T *>(QString::fromUtf8(name));
    check(widget != nullptr, name); return widget;
}
static void click(QWidget *widget)
{
    check(widget && widget->isVisible() && widget->isEnabled(), "pointer target available");
    check(widget->visibleRegion().contains(widget->rect().center()), "pointer target not clipped");
    QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, widget->rect().center());
}
static void toggleFilter()
{
    auto *list = child<QListWidget>("studioMixerEffectList");
    check(list->count() == 1, "inline chain contains real audio filter");
    QStyleOptionViewItem option; option.initFrom(list->viewport());
    option.rect = list->visualItemRect(list->item(0)); option.features = QStyleOptionViewItem::HasCheckIndicator;
    const QRect indicator = list->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, list->viewport());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, indicator.center());
}
static void addCurated(const QString &id)
{
    click(child<QPushButton>("studioMixerAddEffect"));
    auto *picker = window->findChild<QWidget *>("studioEffectPicker");
    check(picker && picker->isVisible(), "curated effect picker opens");
    auto *list = picker->findChild<QListWidget *>("studioEffectCatalog");
    QListWidgetItem *choice = nullptr; int visible = 0;
    for (int row = 0; row < list->count(); ++row) {
        auto *item = list->item(row); if (!item->isHidden()) ++visible;
        if (item->data(Qt::UserRole).toString() == id) choice = item;
    }
    check(visible == 4, "built-in tab exposes four primary effects");
    check(choice && !choice->isHidden(), "requested built-in available");
    picker->grab().save(reportPath + ".picker.png");
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(choice).center());
    struct Find { QString id, uuid; } find{id, {}};
    obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *filter, void *opaque) {
        auto &find = *static_cast<Find *>(opaque);
        if (find.id == QString::fromUtf8(obs_source_get_id(filter))) find.uuid = QString::fromUtf8(obs_source_get_uuid(filter));
    }, &find);
    check(!find.uuid.isEmpty(), "picker adds registered production filter"); extraUUID = find.uuid;
}
static void openExtra()
{
    auto *list = child<QListWidget>("studioMixerEffectList");
    QListWidgetItem *choice = nullptr;
    for (int row = 0; row < list->count(); ++row) if (list->item(row)->data(Qt::UserRole).toString() == extraUUID) choice = list->item(row);
    check(choice, "added effect appears in rack");
    const QRect rect = list->visualItemRect(choice);
    facts["last_open_extra_geometry"] = QJsonObject{{"target_uuid",extraUUID},{"x",rect.x()},{"y",rect.y()},{"width",rect.width()},{"height",rect.height()}, {"viewport_width",list->viewport()->width()},{"viewport_height",list->viewport()->height()}, {"list_height",list->height()}, {"effects_height",child<QWidget>("studioMixerEffects")->height()}};
    channel->grab().save(reportPath + ".before-extra.png");
    check(list->viewport()->rect().contains(rect.center()), "effect row center lies within interactive viewport");
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
}
static void menuAction(const QString &id)
{
    later(150, [id]() {
        auto *menu = child<QToolButton>("studioMixerDevice")->menu();
        check(menu->isVisible(), "footer device popup opens");
        QAction *selected = nullptr;
        for (auto *action : menu->actions()) if (action->data().toString() == id) selected = action;
        check(selected && selected->isEnabled(), "native property device listed");
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(selected).center());
    });
    click(child<QToolButton>("studioMixerDevice"));
}
static const char *fixtureName(void *) { return "Studio mixer test input"; }
static void *fixtureCreate(obs_data_t *settings, obs_source_t *source)
{
    backendDevice = QString::fromUtf8(obs_data_get_string(settings, "device_id")); return source;
}
static void fixtureDestroy(void *) {}
static void fixtureUpdate(void *, obs_data_t *settings) { backendDevice = QString::fromUtf8(obs_data_get_string(settings, "device_id")); }
static void fixtureDefaults(obs_data_t *settings) { obs_data_set_default_string(settings, "device_id", "qa-a"); }
static obs_properties_t *fixtureProperties(void *)
{
    auto *props = obs_properties_create();
    auto *device = obs_properties_add_list(props, "device_id", "Device", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(device, "QA Input A", "qa-a"); obs_property_list_add_string(device, "QA Input B", "qa-b");
    return props;
}
static void run()
{
    window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    check(!obs_frontend_recording_active() && !obs_frontend_streaming_active(), "outputs idle");
    originalStudioMode = obs_frontend_preview_program_mode_active();
    OBSSourceAutoRelease original = obs_frontend_get_current_scene(); originalUUID = QString::fromUtf8(obs_source_get_uuid(original));
    if (originalStudioMode) obs_frontend_set_preview_program_mode(false);
    source = obs_source_create("studio_mixer_test_input", "Studio strip QA", nullptr, nullptr);
    check(bool(source), "synthetic input created with native properties");
    OBSDataAutoRelease settings = obs_data_create(); obs_data_set_string(settings, "preserve", "unchanged"); obs_source_update(source, settings);
    OBSSceneAutoRelease fixtureScene = obs_scene_create("Studio strip QA scene"); scene = obs_source_get_ref(obs_scene_get_source(fixtureScene));
    obs_scene_add(fixtureScene, source);
    filter = obs_source_create("gain_filter", "QA Gain", nullptr, nullptr); check(bool(filter), "native gain filter created"); obs_source_filter_add(source, filter);
    audioTimer = new QTimer(qApp); audioTimer->setInterval(20); audioTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(audioTimer, &QTimer::timeout, qApp, []() {
        if (!source) return;
        float samples[960];
        for (float &sample : samples) sample = .03f * std::sin(2.0 * 3.141592653589793 * 440 * double(sampleIndex++) / 48000);
        obs_source_audio audio{}; audio.data[0] = reinterpret_cast<uint8_t *>(samples); audio.frames = 960;
        audio.samples_per_sec = 48000; audio.speakers = SPEAKERS_MONO; audio.format = AUDIO_FORMAT_FLOAT; audio.timestamp = os_gettime_ns();
        obs_source_output_audio(source, &audio);
    });
    audioTimer->start(); obs_frontend_set_current_scene(scene);
    steps = {
        [] {
            for (auto *widget : window->findChildren<QWidget *>()) if (QByteArray(widget->metaObject()->className()) == "VolumeControl" && widget->objectName() == "Studio strip QA") channel = widget;
            check(channel && channel->isVisible(), "synthetic input has visible native mixer strip");
            check(channel->property("studioStrip").toBool(), "Studio vertical strip active");
            facts["strip_width"] = channel->width(); facts["strip_height"] = channel->height();
            QJsonArray geometry;
            for (const char *id : {"studioMixerFader", "studioMixerMeter", "volLabel", "studioMixerEdit", "studioMixerDevice"}) {
                auto *w = child<QWidget>(id);
                geometry.append(QJsonObject{{"name", id}, {"x", w->mapTo(channel, QPoint()).x()}, {"width", w->width()}, {"minimum", w->minimumWidth()}});
            }
            facts["geometry"] = geometry;
            auto *name = child<QWidget>("studioMixerName"), *effects = child<QWidget>("studioMixerEffects"), *meter = child<QWidget>("studioMixerMeter");
            auto *fader = child<QWidget>("studioMixerFader"), *edit = child<QWidget>("studioMixerEdit"), *device = child<QWidget>("studioMixerDevice");
            const auto point = [](QWidget *widget) { return widget->mapTo(channel, QPoint(0,0)); };
            check(point(name).y() < point(effects).y() && point(effects).y() < point(meter).y(), "header and effects above meter");
            check(point(meter).x() < point(fader).x() && point(fader).x() < point(edit).x(), "meter fader and buttons ordered left to right");
            check(point(device).y() > point(child<QWidget>("studioMixerMute")).y(), "device selector below controls");
            check(edit->width() >= 44 && child<QWidget>("studioMixerMute")->width() >= 44 && child<QWidget>("studioMixerCue")->width() >= 44, "EDIT CUE and MUTE have readable widths");
            check(child<QPushButton>("studioMixerMute")->text() == "MUTE" && child<QPushButton>("studioMixerCue")->text() == "CUE", "explicit MUTE and CUE labels");
            check(child<QLabel>("studioMixerPeak")->text() != QStringLiteral("−∞"), "numeric peak receives real native meter data");
            channel->grab().save(reportPath + ".strip.png"); window->grab().save(reportPath + ".window.png");
            toggleFilter(); check(!obs_source_enabled(filter), "pointer click bypasses real filter");
        },
        [] { window->findChild<QAction *>("actionMainUndo")->trigger(); check(obs_source_enabled(filter), "filter bypass undo restores enabled state"); },
        [] { check(child<QListWidget>("studioMixerEffectList")->item(0)->checkState() == Qt::Checked, "inline chain observes undo"); obs_source_set_name(filter, "Renamed gain"); },
        [] { check(child<QListWidget>("studioMixerEffectList")->item(0)->text() == "Renamed gain", "inline chain observes renamed filter"); menuAction("qa-b"); },
        [] { check(backendDevice == "qa-b", "device click updates native input backend"); window->findChild<QAction *>("actionMainUndo")->trigger(); },
        [] { check(backendDevice == "qa-a", "device undo restores default-valued device"); OBSDataAutoRelease settings = obs_source_get_settings(source); check(QString::fromUtf8(obs_data_get_string(settings, "preserve")) == "unchanged", "device undo preserves unrelated settings"); window->findChild<QAction *>("actionMainRedo")->trigger(); },
        [] { check(backendDevice == "qa-b", "device redo updates backend"); click(child<QPushButton>("studioMixerMute")); check(obs_source_muted(source), "MUTE click updates source"); click(child<QPushButton>("studioMixerMute")); check(!obs_source_muted(source), "MUTE second click restores source"); click(child<QPushButton>("studioMixerCue")); },
        [] { check(obs_source_get_monitoring_type(source) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT, "CUE preserves output and enables monitoring"); click(child<QPushButton>("studioMixerCue")); },
        [] { check(obs_source_get_monitoring_type(source) == OBS_MONITORING_TYPE_NONE, "CUE second click disables monitoring"); click(child<QPushButton>("studioMixerLink")); },
        [] { check(!child<QPushButton>("studioMixerLink")->isChecked(), "link click selects scene-local state"); click(child<QPushButton>("studioMixerLink")); },
        [] { check(child<QPushButton>("studioMixerLink")->isChecked(), "link second click selects global state"); click(child<QPushButton>("studioMixerAddEffect")); },
        [] {
            auto *picker = window->findChild<QWidget *>("studioEffectPicker");
            check(picker && picker->isVisible() && picker->windowModality() == Qt::NonModal, "plus opens nonmodal anchored effect picker");
            for (auto *w : QApplication::topLevelWidgets()) check(!(w->isVisible() && QByteArray(w->metaObject()->className()) == "OBSBasicFilters"), "no separate filters dialog opened");
            auto *catalog = picker->findChild<QListWidget *>("studioEffectCatalog");
            check(catalog && catalog->count() >= 6, "catalog enumerates installed real audio filters");
            QTest::keyClicks(picker->findChild<QLineEdit *>("studioEffectSearch"), "Compressor");
            QListWidgetItem *item = nullptr;
            for (int i = 0; i < catalog->count(); ++i) {
                auto *candidate = catalog->item(i);
                if (candidate->data(Qt::UserRole).toString() == "compressor_filter") item = candidate;
                else check(candidate->isHidden() == (!candidate->data(Qt::UserRole+1).toBool() || !candidate->text().contains("Compressor", Qt::CaseInsensitive)), "search filters within selected effect tab");
            }
            check(item && !item->isHidden(), "compressor found by search");
            QTest::mouseClick(catalog->viewport(), Qt::LeftButton, Qt::NoModifier, catalog->visualItemRect(item).center());
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(!editor || !editor->isVisible(), "adding built-in effect leaves rack visible without opening an editor");
            int count = 0;
            obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *f, void *p) {
                ++*static_cast<int *>(p);
                if (QString::fromUtf8(obs_source_get_id(f)) == "compressor_filter") addedUUID = QString::fromUtf8(obs_source_get_uuid(f));
            }, &count);
            check(count == 2 && !addedUUID.isEmpty(), "one real compressor added to native chain");
            auto *list = child<QListWidget>("studioMixerEffectList");
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(1)).center());
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible() && editor->windowModality() == Qt::NonModal, "effect row opens adjacent nonmodal editor");
            check(editor->findChild<QWidget *>("studioCompactEffectControls"), "built-in compact controls replace generic filter form");
            check(editor->findChild<QDoubleSpinBox *>("knee"), "compressor includes real knee control");
            auto *attack = editor->findChild<QDoubleSpinBox *>("attack_time");
            check(attack && attack->suffix() == " s" && std::abs(attack->value() - .006) < .00001, "attack displayed in seconds with three decimal places");
            check(editor->findChildren<QComboBox *>().size() == 1, "compressor exposes sidechain selector");
            facts["added_filter_uuid"] = addedUUID;
            facts["popup_geometry"] = QJsonObject{{"x", editor->x()}, {"y", editor->y()}, {"width", editor->width()}, {"height", editor->height()}};
            editor->grab().save(reportPath + ".compressor.png");
            window->grab().save(reportPath + ".effects-window.png");
            click(editor->findChild<QPushButton *>("studioEffectClose"));
            window->findChild<QAction *>("actionMainUndo")->trigger();
            check(!obs_get_source_by_uuid(addedUUID.toUtf8().constData()), "add undo removes compressor without retained editor source");
            window->findChild<QAction *>("actionMainRedo")->trigger();
        },
        [] {
            OBSSourceAutoRelease added = obs_get_source_by_uuid(addedUUID.toUtf8().constData());
            check(added && obs_filter_get_parent(added) == source.Get(), "add redo restores same UUID and native attachment");
            auto *list = child<QListWidget>("studioMixerEffectList");
            check(list->count() == 2, "inline strip observes added filter");
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(1)).center());
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "single effect row click opens settings beside strip");
            QDoubleSpinBox *threshold = nullptr;
            for (auto *spin : editor->findChildren<QDoubleSpinBox *>()) if (spin->minimum() == -60) threshold = spin;
            check(threshold, "native compressor threshold control present");
            click(threshold); QTest::keyClick(threshold, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(threshold, "-24"); QTest::keyClick(threshold, Qt::Key_Enter);
            OBSSourceAutoRelease added = obs_get_source_by_uuid(addedUUID.toUtf8().constData());
            OBSDataAutoRelease settings = obs_source_get_settings(added);
            check(std::abs(obs_data_get_double(settings, "threshold") + 24) < .01, "typing threshold previews real DSP setting immediately");
        },
        [] { window->findChild<QAction *>("actionMainUndo")->trigger(); },
        [] {
            OBSSourceAutoRelease added = obs_get_source_by_uuid(addedUUID.toUtf8().constData());
            OBSDataAutoRelease settings = obs_source_get_settings(added);
            check(std::abs(obs_data_get_double(settings, "threshold") + 18) < .01, "property undo restores default compressor threshold");
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            for (auto *spin : editor->findChildren<QDoubleSpinBox *>()) if (spin->minimum() == -60) check(std::abs(spin->value() + 18) < .01, "open editor refreshes after external undo");
            window->findChild<QAction *>("actionMainRedo")->trigger();
        },
        [] {
            OBSSourceAutoRelease added = obs_get_source_by_uuid(addedUUID.toUtf8().constData());
            OBSDataAutoRelease settings = obs_source_get_settings(added);
            check(std::abs(obs_data_get_double(settings, "threshold") + 24) < .01, "property redo restores typed threshold");
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            auto *enabled = editor->findChild<QCheckBox *>("studioEffectEnabled");
            QTest::mouseClick(enabled, Qt::LeftButton, Qt::NoModifier, QPoint(8, enabled->height()/2)); check(!obs_source_enabled(added), "popup enable control bypasses real effect");
            window->findChild<QAction *>("actionMainUndo")->trigger();
        },
        [] {
            OBSSourceAutoRelease added = obs_get_source_by_uuid(addedUUID.toUtf8().constData());
            check(obs_source_enabled(added), "popup bypass undo restores filter");
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor->findChild<QCheckBox *>("studioEffectEnabled")->isChecked(), "popup observes bypass undo");
            click(editor->findChild<QPushButton *>("studioEffectUp"));
            check(obs_source_filter_get_index(source, added) == 1, "move up changes native processing order");
            window->findChild<QAction *>("actionMainUndo")->trigger();
            check(obs_source_filter_get_index(source, added) == 0, "reorder undo restores native index");
        },
        [] {
            auto *list = child<QListWidget>("studioMixerEffectList");
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(1)).center());
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            click(editor->findChild<QPushButton *>("studioEffectRemove"));
            check(!obs_get_source_by_uuid(addedUUID.toUtf8().constData()), "remove button deletes real filter and releases editor");
            window->findChild<QAction *>("actionMainUndo")->trigger();
        },
        [] {
            OBSSourceAutoRelease added = obs_get_source_by_uuid(addedUUID.toUtf8().constData());
            check(added && obs_source_filter_get_index(source, added) == 0, "remove undo restores UUID and chain order");
            OBSDataAutoRelease settings = obs_source_get_settings(added);
            check(std::abs(obs_data_get_double(settings, "threshold") + 24) < .01, "remove undo preserves effect settings");
            added = nullptr;
            window->findChild<QAction *>("actionMainRedo")->trigger();
            check(!obs_get_source_by_uuid(addedUUID.toUtf8().constData()), "remove redo deletes effect");
        },
        [] {
            auto *list = child<QListWidget>("studioMixerEffectList");
            check(list->count() == 1, "inline chain observes removal");
            gainPeakBefore = child<QLabel>("studioMixerPeak")->text().toDouble();
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            auto *gain = editor->findChild<QDoubleSpinBox *>(); check(gain, "gain exposes real processor parameter");
            click(gain); QTest::keyClick(gain, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(gain, "6"); QTest::keyClick(gain, Qt::Key_Enter);
            click(editor->findChild<QPushButton *>("studioEffectClose"));
        },
        [] {
            const double delta = child<QLabel>("studioMixerPeak")->text().toDouble() - gainPeakBefore;
            facts["effect_gain_meter_delta_db"] = delta;
            check(std::abs(delta - 6) < .3, "popup gain edit changes actual measured audio by six dB");
            window->findChild<QAction *>("actionMainUndo")->trigger();
            OBSDataAutoRelease settings = obs_source_get_settings(filter);
            check(std::abs(obs_data_get_double(settings, "db")) < .01, "immediate close flushes pending property undo correctly");
            auto *list = child<QListWidget>("studioMixerEffectList");
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            auto *gain = editor->findChild<QDoubleSpinBox *>();
            click(gain); QTest::keyClick(gain, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(gain, "3"); QTest::keyClick(gain, Qt::Key_Enter);
            auto *enabled = editor->findChild<QCheckBox *>("studioEffectEnabled");
            QTest::mouseClick(enabled, Qt::LeftButton, Qt::NoModifier, QPoint(8, enabled->height()/2));
            check(!obs_source_enabled(filter), "bypass can follow a pending property gesture immediately");
            window->findChild<QAction *>("actionMainUndo")->trigger();
            check(obs_source_enabled(filter), "first undo restores bypass after rapid edit and toggle");
            window->findChild<QAction *>("actionMainUndo")->trigger();
            OBSDataAutoRelease settings = obs_source_get_settings(filter);
            check(std::abs(obs_data_get_double(settings, "db")) < .01, "second undo restores gain with correct rapid action ordering");
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "editor remains open before external filter removal");
            obs_source_filter_remove(source, filter);
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(!editor || !editor->isVisible(), "external filter removal closes stale popup safely");
            obs_source_filter_add(source, filter);
            gainPeakBefore = child<QLabel>("studioMixerPeak")->text().toDouble();
            addCurated("studio_gain_filter");
        },
        [] { openExtra(); },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            auto *gain = editor->findChild<QDoubleSpinBox *>("gain");
            check(gain && editor->findChild<QDial *>("gainDial"), "percentage gain has rotary and typed controls");
            click(gain); QTest::keyClick(gain, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(gain, "200"); QTest::keyClick(gain, Qt::Key_Enter);
            editor->grab().save(reportPath + ".gain.png");
        },
        [] {
            const double delta = child<QLabel>("studioMixerPeak")->text().toDouble() - gainPeakBefore;
            facts["percent_gain_meter_delta_db"] = delta;
            check(std::abs(delta - 6.02) < .3, "percentage gain control doubles actual audio signal");
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            click(editor->findChild<QPushButton *>("studioEffectReset"));
            OBSSourceAutoRelease extra = obs_get_source_by_uuid(extraUUID.toUtf8().constData());
            OBSDataAutoRelease settings = obs_source_get_settings(extra);
            check(std::abs(obs_data_get_double(settings, "gain") - 1) < .0001, "reset restores unity gain immediately");
            window->findChild<QAction *>("actionMainUndo")->trigger();
        },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor->findChild<QDoubleSpinBox *>("gain")->value() == 200, "reset undo refreshes open percentage editor");
            click(editor->findChild<QPushButton *>("studioEffectClose"));
            addCurated("studio_pan_filter");
        },
        [] { openExtra(); },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            auto *pan = editor->findChild<QDoubleSpinBox *>("pan");
            check(pan && editor->findChild<QDial *>("panDial"), "stereo pan rotary editor available");
            click(pan); QTest::keyClick(pan, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(pan, "-100"); QTest::keyClick(pan, Qt::Key_Enter);
            OBSSourceAutoRelease extra = obs_get_source_by_uuid(extraUUID.toUtf8().constData());
            OBSDataAutoRelease settings = obs_source_get_settings(extra);
            check(std::abs(obs_data_get_double(settings, "pan") + 1) < .0001, "pan UI updates actual native filter parameter");
            editor->grab().save(reportPath + ".pan.png");
            click(editor->findChild<QPushButton *>("studioEffectClose"));
            addCurated("studio_voice_filter");
        },
        [] { openExtra(); },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            auto *level = editor->findChild<QComboBox *>("studioEffectVoiceLevel");
            check(level && level->count() == 3, "voice exposes three processing levels");
            QTest::keyClick(level, Qt::Key_End);
            auto *agc = editor->findChild<QCheckBox *>("studioEffectAGC");
            check(agc, "voice exposes automatic gain control"); QTest::mouseClick(agc, Qt::LeftButton, Qt::NoModifier, QPoint(8, agc->height()/2));
            OBSSourceAutoRelease extra = obs_get_source_by_uuid(extraUUID.toUtf8().constData());
            OBSDataAutoRelease settings = obs_source_get_settings(extra);
            check(obs_data_get_int(settings, "level") == 2 && obs_data_get_bool(settings, "enable_agc"), "voice controls update actual suppression and AGC settings");
            editor->grab().save(reportPath + ".voice.png");
            click(editor->findChild<QPushButton *>("studioEffectClose"));
            obs_source_filter_remove(source, filter);
            addCurated("compressor_filter");
        },
        [] { openExtra(); },
        [] {
            auto *editor = window->findChild<QWidget *>("studioEffectEditor");
            check(editor && editor->isVisible(), "effect editor available before accessing controls");
            for (const auto pair : {std::pair<const char *, const char *>{"threshold", "-24"}, {"ratio", "12"}, {"knee", "6"}}) {
                auto *value = editor->findChild<QDoubleSpinBox *>(pair.first); check(value, "compact numeric parameter present");
                click(value); QTest::keyClick(value, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(value, pair.second); QTest::keyClick(value, Qt::Key_Enter);
            }
            auto *attack = editor->findChild<QDoubleSpinBox *>("attack_time");
            click(attack); QTest::keyClick(attack, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(attack, attack->locale().toString(.003, 'f', 3)); QTest::keyClick(attack, Qt::Key_Enter);
            OBSSourceAutoRelease extra = obs_get_source_by_uuid(extraUUID.toUtf8().constData());
            OBSDataAutoRelease settings = obs_source_get_settings(extra);
            check(std::abs(obs_data_get_double(settings, "knee") - 6) < .001, "compact knee updates real compressor parameter");
            check(obs_data_get_int(settings, "attack_time") == 3, "seconds control converts to native milliseconds");
            editor->grab().save(reportPath + ".compressor-final.png");
            click(editor->findChild<QPushButton *>("studioEffectClose"));
        },
        [] {
            auto *list = child<QListWidget>("studioMixerEffectList");
            check(list->count() == 4 && child<QWidget>("studioMixerEffects")->height() >= 99, "four effects and plus fit in expanded rack");
            QStringList before; for (int row=0; row<list->count(); ++row) before.append(list->item(row)->data(Qt::UserRole).toString());
            check(list->model()->moveRow(QModelIndex(), 3, QModelIndex(), 0), "Qt drag model moves effect row");
            QStringList native;
            obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *f, void *p) { static_cast<QStringList *>(p)->append(QString::fromUtf8(obs_source_get_uuid(f))); }, &native);
            check(native.first() == before.last(), "row move changes real native processing order");
            window->findChild<QAction *>("actionMainUndo")->trigger();
            native.clear();
            obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *f, void *p) { static_cast<QStringList *>(p)->append(QString::fromUtf8(obs_source_get_uuid(f))); }, &native);
            check(native == before, "drag reorder undo restores entire chain order");
            window->findChild<QAction *>("actionMainRedo")->trigger();
            native.clear();
            obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *f, void *p) { static_cast<QStringList *>(p)->append(QString::fromUtf8(obs_source_get_uuid(f))); }, &native);
            check(native.first() == before.last(), "drag reorder redo reapplies chain order");
            later(250, [] {
                auto *list = child<QListWidget>("studioMixerEffectList");
                QJsonArray rects;
                for (int row = 0; row < list->count(); ++row) {
                    const QRect rect = list->visualItemRect(list->item(row));
                    rects.append(QJsonObject{{"row", row}, {"x",rect.x()}, {"y",rect.y()}, {"width",rect.width()}, {"height",rect.height()}});
                }
                facts["four_row_rects"] = rects;
                facts["effect_viewport"] = QJsonObject{{"width",list->viewport()->width()}, {"height",list->viewport()->height()}};
                channel->grab().save(reportPath + ".four-effects.png");
                for (int row = 0; row < list->count(); ++row)
                    check(list->viewport()->rect().contains(list->visualItemRect(list->item(row))), "all four effect rows visible without scrolling");
            });
        },
        [] {
            check(QMetaObject::invokeMethod(channel, "setStudioStyle", Qt::DirectConnection, Q_ARG(bool, false)), "switch strip to classic style");
        },
        [] { check(!channel->property("studioStrip").toBool() && child<QToolButton>("studioMixerDevice")->isHidden(), "classic style hides Studio-specific controls"); check(QMetaObject::invokeMethod(channel, "setStudioStyle", Qt::DirectConnection, Q_ARG(bool, true)), "restore Studio style"); },
        [] { check(channel->property("studioStrip").toBool() && child<QToolButton>("studioMixerDevice")->isVisible(), "Studio controls survive layout rebuild"); click(child<QPushButton>("studioMixerMute")); check(obs_source_muted(source), "controls work after rebuilding layout"); click(child<QPushButton>("studioMixerMute"));
                 auto *fader = child<QSlider>("studioMixerFader");
                 const float before = obs_source_get_volume(source);
                 QTest::mousePress(fader, Qt::LeftButton, Qt::NoModifier, QPoint(fader->width()/2, fader->height()/2));
                 QTest::mouseMove(fader, QPoint(fader->width()/2, fader->height()*2/3));
                 QTest::mouseRelease(fader, Qt::LeftButton, Qt::NoModifier, QPoint(fader->width()/2, fader->height()*2/3));
                 check(std::abs(obs_source_get_volume(source)-before) > .01, "mouse drag changes native fader gain");
                 window->findChild<QAction *>("actionMainUndo")->trigger();
                 check(std::abs(obs_source_get_volume(source)-before) < .001, "fader drag supports undo after layout rebuild");
                 channel->grab().save(reportPath + ".after.png");
                 auto *dock = window->findChild<QDockWidget *>("mixerDock"); check(dock && dock->titleBarWidget(), "mixer header available");
                 click(dock->titleBarWidget()->findChild<QToolButton *>()); },
        [] { bool found = false; for (auto *widget : QApplication::topLevelWidgets()) if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == "OBSBasicSourceSelect") { found = true; widget->close(); break; } check(found, "mixer header plus opens source selector"); }
    };
    next();
}
static void event(enum obs_frontend_event event, void *) {
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) return;
    later(1800, []() {
        if (!qEnvironmentVariableIsSet("OBS_STUDIO_MIXER_CAPTURE_ONLY")) { run(); return; }
        window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        facts["capture_only"] = true;
        for (auto *widget : window->findChildren<QWidget *>())
            if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == "VolumeControl" && widget->objectName() == QString::fromUtf8("Studio — microfone")) {
                widget->grab().save(reportPath + ".microphone.png");
                check(true, "original microphone channel captured without changing inputs");
                finish(); return;
            }
        throw std::runtime_error("original microphone strip not visible");
    });
}
bool obs_module_load()
{
    reportPath = qEnvironmentVariable("OBS_STUDIO_MIXER_STRIP_REPORT"); if (reportPath.isEmpty()) return false;
    if (qEnvironmentVariableIsSet("OBS_STUDIO_MIXER_CAPTURE_ONLY")) {
        obs_frontend_add_event_callback(event, nullptr); return true;
    }
    obs_source_info info{}; info.id = "studio_mixer_test_input"; info.type = OBS_SOURCE_TYPE_INPUT; info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = fixtureName; info.create = fixtureCreate; info.destroy = fixtureDestroy; info.update = fixtureUpdate; info.get_defaults = fixtureDefaults; info.get_properties = fixtureProperties;
    obs_register_source(&info); obs_frontend_add_event_callback(event, nullptr); return true;
}
void obs_module_unload() { obs_frontend_remove_event_callback(event, nullptr); if (audioTimer) { audioTimer->stop(); delete audioTimer; audioTimer = nullptr; } source = nullptr; scene = nullptr; filter = nullptr; }
