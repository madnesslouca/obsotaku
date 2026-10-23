/* Opt-in real Studio dock callback, private config only. No endpoint is
 * unplugged and no audio is captured, monitored, recorded or transmitted. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/bmem.h>
#include <util/config-file.h>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <memory>
#include <stdexcept>

OBS_DECLARE_MODULE()
namespace {
QString root, configRoot;
QMainWindow *window = nullptr;
QJsonArray checks;
QJsonObject facts;
bool done = false;
OBSSourceAutoRelease source;
void check(bool ok, const char *label)
{
	checks.append(QJsonObject{{"check", label}, {"passed", ok}});
	if (!ok)
		throw std::runtime_error(label);
}
QJsonObject endpoint()
{
	const char *name = nullptr, *id = nullptr;
	obs_get_audio_monitoring_device(&name, &id);
	return {{"name", QString::fromUtf8(name ? name : "")}, {"id", QString::fromUtf8(id ? id : "")}};
}
QJsonObject settings()
{
	auto *profile = obs_frontend_get_profile_config();
	return {{"name", QString::fromUtf8(config_get_string(profile, "Audio", "MonitoringDeviceName"))},
		{"id", QString::fromUtf8(config_get_string(profile, "Audio", "MonitoringDeviceId"))}};
}
QJsonObject audio()
{
	return {{"mute", obs_source_muted(source)}, {"mixers", int(obs_source_get_audio_mixers(source))},
		{"monitor", int(obs_source_get_monitoring_type(source))}, {"volume", double(obs_source_get_volume(source))},
		{"sync", double(obs_source_get_sync_offset(source))}};
}
void finish(const QString &error = {})
{
	if (done)
		return;
	done = true;
	source = nullptr;
	QSaveFile file(root + "/runtime.json");
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{{"passed", error.isEmpty()}, {"error", error}, {"checks", checks},
						  {"facts", facts}, {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
						  {"scope", "Real StudioAudioDock Refresh/popup/activated callback and isolated profile save/read; synthetic stale endpoint only, no physical hotplug, no captured audio, no output media."}}).toJson());
		file.commit();
	}
	QTimer::singleShot(250, qApp, [] { if (window) window->close(); });
}
void run()
{
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window != nullptr, "native main window available");
		char *raw = obs_frontend_get_current_profile_path();
		const QString profilePath = QFileInfo(QString::fromUtf8(raw)).canonicalFilePath();
		bfree(raw);
		const QString privatePath = QFileInfo(configRoot).canonicalFilePath();
		check(!privatePath.isEmpty() && profilePath.startsWith(privatePath + '/'), "effective profile confined to private root");
		facts["profile_path"] = profilePath;
		check(!obs_frontend_streaming_active() && !obs_frontend_recording_active() && !obs_frontend_replay_buffer_active(), "all public frontend outputs idle");
		for (unsigned channel = 1; channel <= 5; ++channel) {
			OBSSourceAutoRelease global = obs_get_output_source(channel);
			check(!global, "private fixture has no global physical audio capture");
		}
		check(obs_audio_monitoring_available(), "monitor backend available without monitored sources");
		obs_source_info info{};
		info.id = "studio-monitoring-device-fixture";
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_AUDIO;
		info.get_name = [](void *) { return "Private silent monitoring fixture"; };
		info.create = [](obs_data_t *, obs_source_t *) -> void * { return new int(0); };
		info.destroy = [](void *ptr) { delete static_cast<int *>(ptr); };
		obs_register_source(&info);
		source = obs_source_create(info.id, "Private silent monitoring fixture", nullptr, nullptr);
		check(bool(source), "silent synthetic source created without device capture");
		obs_source_set_audio_mixers(source, 0x2b);
		obs_source_set_volume(source, .37f);
		obs_source_set_sync_offset(source, 123000000);
		const auto originalAudio = audio();
		const auto originalEndpoint = endpoint();
		const auto originalSettings = settings();
		auto *dock = window->findChild<QDockWidget *>("studioAudioDock");
		check(dock != nullptr, "actual Studio audio dock found");
		dock->show();
		dock->raise();
		auto *combo = dock->findChild<QComboBox *>("studioAudioMonitorDevice");
		auto *refresh = dock->findChild<QPushButton *>("studioAudioRefreshDevices");
		auto *feedback = dock->findChild<QLabel *>("studioAudioFeedback");
		check(combo && refresh && feedback, "actual monitoring combo refresh and feedback found");
		refresh->click();
		check(combo->findData("default") >= 0, "real Refresh button enumerates logical default");
		check(endpoint() == originalEndpoint && settings() == originalSettings && audio() == originalAudio, "refresh changes no endpoint profile or source routing");
		combo->showPopup();
		combo->hidePopup();
		check(endpoint() == originalEndpoint && settings() == originalSettings && audio() == originalAudio, "opening and dismissing actual popup is read only");
		const QString fake = "unplugged-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
		combo->addItem("Previously available fixture endpoint", fake);
		combo->setCurrentIndex(combo->count() - 1);
		feedback->clear();
		check(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, combo->currentIndex())), "stale activation delivered to actual production callback");
		check(endpoint() == originalEndpoint, "stale endpoint refused before core mutation");
		check(settings() == originalSettings, "stale endpoint refused before profile mutation");
		check(audio() == originalAudio, "stale endpoint leaves mute mixes monitoring volume delay untouched");
		check(feedback->text() == QString::fromUtf8(obs_frontend_get_locale_string("Studio.AudioMonitorDeviceError")), "stale endpoint refusal has exact error feedback");
		check(combo->findData(fake) < 0 && combo->currentData().toString() == originalEndpoint["id"].toString(), "stale row removed and effective endpoint restored in UI");
		const int index = combo->findData("default");
		check(index >= 0, "default available for explicit persistence gesture");
		const QString canonicalDefault = QString::fromUtf8(obs_frontend_get_locale_string(
			"Basic.Settings.Advanced.Audio.MonitoringDevice.Default"));
		check(!canonicalDefault.isEmpty(), "canonical default label available from production locale");
		const QString decoratedDefault = "Display decoration only - not a persisted device name";
		combo->setItemText(index, decoratedDefault);
		check(combo->itemData(index).toString() == "default" && combo->itemText(index) == decoratedDefault,
		      "default row decorated without changing its stable endpoint ID");
		combo->setCurrentIndex(index);
		check(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index)), "explicit default activation delivered to actual callback");
		check(endpoint()["id"] == "default", "default selected in actual core");
		check(settings()["id"] == "default", "canonical default ID saved in effective profile");
		check(settings()["name"].toString() == canonicalDefault,
		      "actual callback saves canonical locale name instead of decorated row text");
		check(combo->currentText() == canonicalDefault,
		      "successful activation refreshes decorated row to canonical device label");
		config_t *disk = nullptr;
		check(config_open(&disk, (profilePath + "/basic.ini").toUtf8().constData(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS, "profile reopened from disk without injecting defaults");
		std::unique_ptr<config_t, decltype(&config_close)> owned(disk, config_close);
		check(QString::fromUtf8(config_get_string(disk, "Audio", "MonitoringDeviceId")) == "default", "disk contains stable default ID");
		check(QString::fromUtf8(config_get_string(disk, "Audio", "MonitoringDeviceName")) == settings()["name"].toString(), "disk contains canonical selected name");
		check(QString::fromUtf8(config_get_string(disk, "Audio", "MonitoringDeviceName")) == canonicalDefault,
		      "disk name matches independent canonical expectation after decorated selection");
		check(audio() == originalAudio, "accepted selection preserves source CUE mode and program fields");
		check(feedback->text() == QString::fromUtf8(obs_frontend_get_locale_string("Studio.AudioMonitorDeviceSaved")), "successful save has exact saved feedback");
		facts["feedback_after_save"] = feedback->text();
		facts["default_saved"] = settings();
		facts["decorated_row_text"] = decoratedDefault;
		facts["canonical_default_name"] = canonicalDefault;
		facts["source_audio_preserved"] = originalAudio;
		const QString obstacle = profilePath + "/basic.ini.tmp";
		check(!QFileInfo::exists(obstacle) && QDir().mkdir(obstacle), "owned empty temporary-path directory injects safe-save failure");
		const bool invoked = QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, combo->currentIndex()));
		// Remove our empty directory before assertions or shutdown so even a
		// regression does not obstruct the private application's final save.
		const bool removed = QDir().rmdir(obstacle);
		check(removed, "owned failure-injection directory removed before teardown");
		check(invoked, "default activation exercises production safe-save failure branch");
		check(feedback->text() == QString::fromUtf8(obs_frontend_get_locale_string("Studio.AudioMonitorDeviceNotSaved")), "failed safe-save has exact not-saved feedback");
		check(endpoint()["id"] == "default" && settings()["id"] == "default", "failed persistence retains selected runtime and memory preference without false rollback");
		check(audio() == originalAudio, "failed persistence leaves source program and CUE fields untouched");
		facts["feedback_after_failed_save"] = feedback->text();
		finish();
	} catch (const std::exception &error) {
		finish(QString::fromUtf8(error.what()));
	}
}
void event(obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		QTimer::singleShot(1800, qApp, [] { if (!done) run(); });
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_MONITORING_DEVICE_ROOT");
	configRoot = qEnvironmentVariable("OBS_STUDIO_MONITORING_DEVICE_CONFIG_ROOT");
	if (root.isEmpty() || configRoot.isEmpty() || QFileInfo::exists(root + "/runtime.json") || !QDir().mkpath(root))
		return false;
	obs_frontend_add_event_callback(event, nullptr);
	QTimer::singleShot(40000, qApp, [] { if (!done) finish("private monitoring fixture timed out"); });
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
