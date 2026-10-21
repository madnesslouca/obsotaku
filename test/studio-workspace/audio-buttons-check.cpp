/* Real Qt pointer events, scoped to an inert source and opt-in test runtime. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QApplication>
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
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QAbstractItemView>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <functional>
#include <stdexcept>
#include <cmath>
#include <util/config-file.h>

OBS_DECLARE_MODULE()
static QString reportPath, sourceUuid;
static QMainWindow *window;
static OBSSourceAutoRelease fixture;
static QJsonArray checks;
static QJsonObject facts;
static bool finished = false;

static void check(bool value, const char *label)
{
    checks.append(QJsonObject{{"check", label}, {"passed", value}});
    if (!value) throw std::runtime_error(label);
}
static void finish(const QString &error = {})
{
    if (finished) return;
    finished = true;
    if (fixture) obs_source_remove(fixture);
    fixture = nullptr;
    QFile file(reportPath);
    if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
        {"passed", error.isEmpty()}, {"error", error}, {"checks", checks}, {"facts", facts},
        {"scope", "actual Qt mouse events; temporary inactive source only; no recording or streaming"}
    }).toJson());
}
static void later(int ms, std::function<void()> callback)
{
    QTimer::singleShot(ms, qApp, [callback]() {
        if (finished) return;
        try { callback(); } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); }
    });
}
template<typename T> static T *child(const char *name)
{
    auto *widget = window->findChild<T *>(QString::fromUtf8(name));
    check(widget != nullptr, name); return widget;
}
static bool reachable(QWidget *widget)
{
    return widget->isVisible() && widget->isEnabled() && widget->visibleRegion().contains(widget->rect().center());
}
static void reveal(QWidget *widget)
{
    for (QWidget *parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) scroll->ensureWidgetVisible(widget);
}
static void click(QWidget *widget)
{
    reveal(widget); check(reachable(widget), "control reachable before pointer click");
    QPoint pos = widget->rect().center();
    if (auto *box = qobject_cast<QCheckBox *>(widget)) pos.setX(8);
    QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, pos);
}
static void staleSelection()
{
    obs_source_remove(fixture); fixture = nullptr;
    later(500, []() {
        click(child<QPushButton>("studioAudioRoutingButton"));
        later(300, []() {
            auto *gain = child<QDoubleSpinBox>("studioAudioGain");
            facts["controls_enabled_after_source_removal"] = gain->isEnabled();
            check(gain->isEnabled(), "Audio button recovers selection after source is removed");
            check(reachable(gain), "Audio button reveals gain after panel was scrolled");
            auto *table = child<QTableWidget>("studioAudioRouting");
            const auto uuid = table->item(table->currentRow(), 0)->data(Qt::UserRole).toString();
            OBSSourceAutoRelease recovered = obs_get_source_by_uuid(uuid.toUtf8().constData());
            check(recovered && !obs_source_removed(recovered), "recovered selection references a real source");
            check(window->findChild<QWidget *>("studioAudioLevelMeter") != nullptr, "meter reattaches after removal");
            child<QDockWidget>("studioAudioDock")->grab().save(reportPath + ".png");
            finish();
        });
    });
}
static void runButtons()
{
    click(child<QPushButton>("studioAudioRoutingButton"));
    later(350, []() {
        auto *dock = child<QDockWidget>("studioAudioDock");
        facts["panel_height_after_audio_click"] = dock->height();
        facts["gain_initially_reachable"] = reachable(child<QDoubleSpinBox>("studioAudioGain"));
        check(dock->isVisible(), "pointer click opens audio panel");
        if (!qEnvironmentVariableIsSet("OBS_STUDIO_AUDIO_BUTTON_DIAGNOSTIC")) {
            check(facts["gain_initially_reachable"].toBool(), "gain visible immediately after Audio click");
            check(window->findChild<QWidget *>("studioAudioLevelMeter") != nullptr, "native meter attached to selected source");
            auto *devices = child<QComboBox>("studioAudioMonitorDevice");
            check(devices->isEnabled() && devices->findData("default") >= 0, "native monitoring devices enumerated");
            const QString previousDevice = devices->currentData().toString();
            click(child<QPushButton>("studioAudioRefreshDevices"));
            check(devices->currentData().toString() == previousDevice, "refresh preserves monitoring device");
            const int previousIndex = devices->currentIndex();
            check(devices->count() > 1, "monitoring backend exposes a physical output");
            reveal(devices); devices->setFocus();
            QTest::keyClick(devices, previousIndex == 0 ? Qt::Key_End : Qt::Key_Home);
            const char *name = nullptr, *id = nullptr;
            obs_get_audio_monitoring_device(&name, &id);
            const QString chosenDevice = devices->currentData().toString();
            const bool backendChanged = QString::fromUtf8(id) == chosenDevice && chosenDevice != previousDevice;
            auto *config = obs_frontend_get_profile_config();
            const bool saved = QString::fromUtf8(config_get_string(config, "Audio", "MonitoringDeviceId")) == chosenDevice;
            const bool feedback = !child<QLabel>("studioAudioFeedback")->text().isEmpty();
            // Restore before assertions so even a failed check leaves the original output selected.
            QTest::keyClick(devices, Qt::Key_Home);
            for (int i = 0; i < previousIndex; ++i) QTest::keyClick(devices, Qt::Key_Down);
            obs_get_audio_monitoring_device(&name, &id);
            check(QString::fromUtf8(id) == previousDevice, "original monitoring device restored");
            check(backendChanged, "device selection reaches native monitoring backend");
            check(saved, "monitoring device saved in active profile");
            facts["monitoring_device_id"] = previousDevice;
            check(feedback, "device selection provides feedback");
        }
        OBSDataAutoRelease settings = obs_data_create();
        fixture = obs_source_create("ffmpeg_source", "Studio click test — inactive audio", settings, nullptr);
        check(bool(fixture), "inert source created"); sourceUuid = QString::fromUtf8(obs_source_get_uuid(fixture));
        check(QMetaObject::invokeMethod(window, "OpenStudioAudioRouting", Qt::DirectConnection, Q_ARG(QString, sourceUuid)), "focus source in audio panel");
        later(400, []() {
            if (qEnvironmentVariableIsSet("OBS_STUDIO_AUDIO_BUTTON_DIAGNOSTIC")) { staleSelection(); return; }
            auto *table = child<QTableWidget>("studioAudioRouting");
            const int row = table->currentRow();
            check(row >= 0 && table->item(row, 0)->data(Qt::UserRole).toString() == sourceUuid, "inert source is selected");
            const bool initialMute = obs_source_muted(fixture);
            click(child<QCheckBox>("studioAudioMono"));
            check(obs_source_get_flags(fixture) & OBS_SOURCE_FLAG_FORCE_MONO, "pointer click enables mono");
            click(child<QCheckBox>("studioAudioMono"));
            check(!(obs_source_get_flags(fixture) & OBS_SOURCE_FLAG_FORCE_MONO), "pointer click disables mono");
            auto *gain = child<QDoubleSpinBox>("studioAudioGain");
            reveal(gain); gain->setFocus(); gain->selectAll();
            QTest::keyClicks(gain, "-9"); QTest::keyClick(gain, Qt::Key_Return);
            check(std::abs(obs_mul_to_db(obs_source_get_volume(fixture)) + 9.) < .1, "keyboard edits source gain");
            const uint32_t originalMask = obs_source_get_audio_mixers(fixture);
            reveal(table); table->scrollToItem(table->item(row, 2));
            QStyleOptionViewItem option; option.initFrom(table->viewport());
            option.rect = table->visualItemRect(table->item(row, 2));
            option.features = QStyleOptionViewItem::HasCheckIndicator;
            const QRect indicator = table->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, table->viewport());
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, indicator.center());
            check(obs_source_get_audio_mixers(fixture) == (originalMask ^ 2u), "pointer click toggles output track");
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, indicator.center());
            check(obs_source_get_audio_mixers(fixture) == originalMask, "pointer click restores output track");
            auto *monitor = child<QComboBox>("studioAudioMonitor");
            reveal(monitor); monitor->setFocus();
            QTest::keyClick(monitor, Qt::Key_End);
            check(obs_source_get_monitoring_type(fixture) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT, "keyboard enables Cue and output");
            QTest::keyClick(monitor, Qt::Key_Home);
            check(obs_source_get_monitoring_type(fixture) == OBS_MONITORING_TYPE_NONE, "keyboard disables Cue");
            click(child<QCheckBox>("studioAudioMute"));
            check(obs_source_muted(fixture) != initialMute, "pointer click changes source mute");
            click(child<QCheckBox>("studioAudioMute"));
            check(obs_source_muted(fixture) == initialMute, "second pointer click restores source mute");
            click(child<QPushButton>("studioAudioVoicePreset"));
            check(obs_source_filter_count(fixture) == 3, "pointer click adds native voice effects");
            check(!child<QLabel>("studioAudioFeedback")->text().isEmpty(), "voice chain provides visible feedback");
            click(child<QPushButton>("studioAudioVoicePreset"));
            check(obs_source_filter_count(fixture) == 3, "second pointer click keeps one voice chain");
            check(!child<QLabel>("studioAudioFeedback")->text().isEmpty(), "duplicate voice preset explains existing chain");
            click(child<QPushButton>("studioAudioEffects"));
            later(350, []() {
                bool found = false;
                for (auto *widget : QApplication::topLevelWidgets())
                    if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == "OBSBasicFilters") {
                        found = true; widget->close(); break;
                    }
                check(found, "pointer click opens source effects editor");
                staleSelection();
            });
        });
    });
}
static void event(enum obs_frontend_event event, void *)
{
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) return;
    later(1200, []() {
        window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        check(!obs_frontend_recording_active() && !obs_frontend_streaming_active(), "no active outputs");
        runButtons();
    });
}
bool obs_module_load()
{
    reportPath = qEnvironmentVariable("OBS_STUDIO_AUDIO_BUTTON_REPORT");
    if (reportPath.isEmpty()) return false;
    obs_frontend_add_event_callback(event, nullptr); return true;
}
void obs_module_unload() { obs_frontend_remove_event_callback(event, nullptr); fixture = nullptr; }
