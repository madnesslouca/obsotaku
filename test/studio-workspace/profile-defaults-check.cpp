/* Opt-in real frontend profile regression. Never starts outputs or opens
 * Settings: inspecting config/default/user values first avoids a constructor
 * crash in an unfixed application. All mutations use private QA profiles. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/bmem.h>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>

OBS_DECLARE_MODULE()
namespace {
struct Key {
	const char *section;
	const char *name;
	const char *explicitValue;
};
constexpr std::array<Key, 4> keys{{{"SimpleOutput", "StreamEncoder", "x264"},
				   {"SimpleOutput", "RecEncoder", "x264"},
				   {"AdvOut", "AudioEncoder", "ffmpeg_aac"},
				   {"AdvOut", "RecAudioEncoder", "none"}}};
QString root, configRoot, expectation, originalProfile, freshProfile, userProfile, terminalError;
QMainWindow *window = nullptr;
QJsonArray checks, observations;
QJsonObject baseline;
QStringList temporaryProfiles;
bool done = false, ready = false, shuttingDown = false, profileOperation = false, cleaning = false;
bool fixtureCompleted = false, originalRestored = false, defectReproduced = false;
void finish(QString error = {});
void cleanup();

QString currentProfile()
{
	char *raw = obs_frontend_get_current_profile();
	const QString name = QString::fromUtf8(raw);
	bfree(raw);
	return name;
}
void report()
{
	QString error = terminalError;
	if (error.isEmpty() && expectation == "before" && defectReproduced) {
		error = "Expected baseline defect reproduced: duplicate loses module-aware encoder defaults";
	}
	QSaveFile file(root + "/runtime.json");
	if (file.open(QIODevice::WriteOnly)) {
		file.write(
			QJsonDocument(
				QJsonObject{
					{"passed", fixtureCompleted && error.isEmpty() && expectation == "after"},
					{"fixture_completed", fixtureCompleted},
					{"error", error},
					{"checks", checks},
					{"observations", observations},
					{"expectation", expectation},
					{"baseline_defect_reproduced", defectReproduced},
					{"original_profile_restored", originalRestored},
					{"profile_operation_on_stack", profileOperation},
					{"original_profile", originalProfile},
					{"temporary_profiles", QJsonArray::fromStringList(temporaryProfiles)},
#if defined(Q_OS_WIN)
					{"compile_target", "Windows"},
#elif defined(Q_OS_LINUX)
					{"compile_target", "Linux"},
#else
					{"compile_target", "other"},
#endif
					{"native_environment_verified", false},
					{"scope",
					 "Real Qt/frontend private profile duplicate and switch; inspect effective/default/user codec values and persistence. No Settings dialog, outputs, network or device capture. Native execution evidence belongs to the parent runner."}})
				.toJson());
		file.commit();
	}
}
void check(bool pass, const char *name)
{
	checks.append(QJsonObject{{"check", name},
				  {"passed", pass},
				  {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
	QSaveFile file(root + "/progress.json");
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{{"checks", checks},
						     {"last_check", name},
						     {"profile_operation_on_stack", profileOperation}})
				   .toJson());
		file.commit();
	}
	if (!pass) {
		throw std::runtime_error(name);
	}
}
QJsonObject snapshot(const char *label)
{
	auto *config = obs_frontend_get_profile_config();
	QJsonObject result{{"label", label},
			   {"profile", currentProfile()},
			   {"mode", QString::fromUtf8(config_get_string(config, "Output", "Mode"))}};
	QJsonObject values;
	for (const auto &key : keys) {
		const char *effective = config_get_string(config, key.section, key.name);
		const char *fallback = config_get_default_string(config, key.section, key.name);
		const bool user = config_has_user_value(config, key.section, key.name);
		values[QString("%1/%2").arg(key.section, key.name)] = QJsonObject{
			{"user_present", user},
			{"effective_is_null", effective == nullptr},
			{"default_is_null", fallback == nullptr},
			{"effective",
			 effective ? QJsonValue(QString::fromUtf8(effective)) : QJsonValue(QJsonValue::Null)},
			{"default", fallback ? QJsonValue(QString::fromUtf8(fallback)) : QJsonValue(QJsonValue::Null)},
			{"user",
			 user && effective ? QJsonValue(QString::fromUtf8(effective)) : QJsonValue(QJsonValue::Null)}};
	}
	result["values"] = values;
	observations.append(result);
	return result;
}
QJsonObject diskSnapshot(const char *label)
{
	char *raw = obs_frontend_get_current_profile_path();
	const QString path = QString::fromUtf8(raw) + "/basic.ini";
	bfree(raw);
	config_t *rawConfig = nullptr;
	check(config_open(&rawConfig, path.toUtf8().constData(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS,
	      "current private profile file can be inspected without applying defaults");
	std::unique_ptr<config_t, decltype(&config_close)> fileConfig(rawConfig, config_close);
	QJsonObject values;
	for (const auto &key : keys) {
		const char *entry = config_get_string(fileConfig.get(), key.section, key.name);
		values[QString("%1/%2").arg(key.section, key.name)] = QJsonObject{
			{"present", config_has_user_value(fileConfig.get(), key.section, key.name)},
			{"value", entry ? QJsonValue(QString::fromUtf8(entry)) : QJsonValue(QJsonValue::Null)}};
	}
	QFile file(path);
	check(file.open(QIODevice::ReadOnly), "private profile bytes can be hashed without changing them");
	QJsonObject result{
		{"label", label},
		{"profile", currentProfile()},
		{"kind", "read_only_disk_without_defaults"},
		{"sha256",
		 QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex())},
		{"values", values}};
	observations.append(result);
	return result;
}
QJsonObject value(const QJsonObject &observation, const Key &key)
{
	return observation["values"].toObject()[QString("%1/%2").arg(key.section, key.name)].toObject();
}
bool hasProfile(const QString &name)
{
	char **profiles = obs_frontend_get_profiles();
	bool found = false;
	if (profiles) {
		for (char **p = profiles; *p; ++p) {
			found = found || QString::fromUtf8(*p) == name;
		}
	}
	bfree(profiles);
	return found;
}
void operation(const std::function<void()> &fn)
{
	profileOperation = true;
	try {
		fn();
	} catch (...) {
		profileOperation = false;
		throw;
	}
	profileOperation = false;
}
void cleanup()
{
	if (shuttingDown || cleaning) {
		return;
	}
	if (profileOperation) {
		// A frontend call can dispatch nested events. Never restore its config
		// while that call is still on the stack, even after a deadline fails.
		report();
		return;
	}
	cleaning = true;
	try {
		if (ready) {
			check(!obs_video_active(), "no output is active before restoring the private profile");
			check(!qApp->activeModalWidget(),
			      "Settings and other modal dialogs remain closed before restore");
			operation([] { obs_frontend_set_current_profile(originalProfile.toUtf8().constData()); });
			if (shuttingDown) {
				return;
			}
			check(currentProfile() == originalProfile, "original private profile is restored");
			const auto restored = snapshot("original_restored");
			for (const auto &key : keys) {
				check(value(restored, key) == value(baseline, key),
				      "original user default and effective codec values are unchanged");
			}
			originalRestored = true;
			for (auto it = temporaryProfiles.crbegin(); it != temporaryProfiles.crend(); ++it) {
				if (hasProfile(*it)) {
					const QString name = *it;
					operation([name] { obs_frontend_delete_profile(name.toUtf8().constData()); });
					if (shuttingDown) {
						return;
					}
					check(!hasProfile(name), "only the owned temporary profile is deleted");
				}
			}
			check(!obs_video_active(), "profile regression started no encoders or outputs");
		}
		fixtureCompleted = terminalError.isEmpty();
	} catch (const std::exception &e) {
		if (terminalError.isEmpty()) {
			terminalError = QString::fromUtf8(e.what());
		}
	}
	report();
}
void finish(QString error)
{
	if (done) {
		return;
	}
	done = true;
	terminalError = error;
	cleanup();
}
void duplicate(const QString &name)
{
	temporaryProfiles.append(name);
	operation([name] { obs_frontend_duplicate_profile(name.toUtf8().constData()); });
	if (done && !shuttingDown) {
		QTimer::singleShot(0, qApp, cleanup);
	}
}
void run()
{
	try {
		window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		check(window && QThread::currentThread() == qApp->thread(),
		      "profile probe runs on the frontend thread");
		check(!obs_video_active(), "application is idle before profile mutation");
		check(!qApp->activeModalWidget(), "no Settings or other modal dialog is open before the probe");
		const QString expected = QFileInfo(configRoot).canonicalFilePath();
		check(!configRoot.isEmpty() && !expected.isEmpty(), "private configuration root exists");
		char *rawPath = obs_frontend_get_current_profile_path();
		const QString path = QFileInfo(QString::fromUtf8(rawPath)).canonicalFilePath();
		bfree(rawPath);
		check(path.startsWith(expected + "/"), "effective profile is inside the private root before mutation");
		for (const char *key : {"Configuration", "Profiles", "SceneCollections", "PluginManagerSettings"}) {
			auto *app = obs_frontend_get_app_config();
			if (config_has_user_value(app, "Locations", key)) {
				const QString raw = QString::fromUtf8(config_get_string(app, "Locations", key));
				check(!raw.isEmpty() && QFileInfo(raw).canonicalFilePath() == expected,
				      "explicit Locations targets only the private root");
			}
		}
		baseline = snapshot("startup_baseline");
		check(baseline["mode"] == "Simple", "private baseline uses Simple mode");
		diskSnapshot("startup_disk_before_duplicate");
		for (const auto &key : keys) {
			const auto entry = value(baseline, key);
			check(!entry["effective_is_null"].toBool() && !entry["default_is_null"].toBool() &&
				      entry["effective"] == entry["default"] &&
				      !entry["effective"].toString().isEmpty(),
			      "startup has valid module defaults and matching effective codec values");
		}
		originalProfile = currentProfile();
		ready = true;
		const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
		freshProfile = "Defaults fresh QA " + suffix;
		userProfile = "Defaults user QA " + suffix;
		duplicate(freshProfile);
		if (done || shuttingDown) {
			return;
		}
		check(currentProfile() == freshProfile, "native duplicate activates the owned fresh profile");
		const auto fresh = snapshot("fresh_duplicate");
		diskSnapshot("fresh_duplicate_disk");
		unsigned missing = 0;
		for (const auto &key : keys) {
			const auto entry = value(fresh, key);
			missing += entry["default_is_null"].toBool();
			if (expectation == "after") {
				check(!entry["effective_is_null"].toBool() &&
					      entry["effective"] == value(baseline, key)["effective"] &&
					      entry["default"] == value(baseline, key)["default"],
				      "fresh duplicate retains module-aware default and effective codec values");
			}
		}
		defectReproduced = missing == keys.size();
		if (expectation == "before") {
			check(defectReproduced,
			      "baseline reproduces all four lost module-aware defaults without opening Settings");
		}
		auto *config = obs_frontend_get_profile_config();
		for (const auto &key : keys) {
			config_set_string(config, key.section, key.name, key.explicitValue);
		}
		check(config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS,
		      "explicit values are saved only in the owned profile");
		snapshot("explicit_user_source");
		duplicate(userProfile);
		if (done || shuttingDown) {
			return;
		}
		check(currentProfile() == userProfile,
		      "second native duplicate activates the owned explicit-value profile");
		const auto explicitCopy = snapshot("explicit_user_duplicate");
		for (const auto &key : keys) {
			const auto entry = value(explicitCopy, key);
			check(entry["user_present"].toBool() && entry["effective"] == key.explicitValue,
			      "explicit user codec values survive duplication and override defaults");
			if (expectation == "after") {
				check(entry["default"] == value(baseline, key)["default"],
				      "reapplying defaults preserves user selection while restoring the module default");
			}
		}
		operation([] { obs_frontend_set_current_profile(originalProfile.toUtf8().constData()); });
		if (done || shuttingDown) {
			if (done && !shuttingDown) {
				QTimer::singleShot(0, qApp, cleanup);
			}
			return;
		}
		operation([] { obs_frontend_set_current_profile(userProfile.toUtf8().constData()); });
		if (done || shuttingDown) {
			if (done && !shuttingDown) {
				QTimer::singleShot(0, qApp, cleanup);
			}
			return;
		}
		check(currentProfile() == userProfile, "explicit-value profile reopens through the native switch path");
		const auto reloaded = snapshot("explicit_user_reopened");
		for (const auto &key : keys) {
			const auto entry = value(reloaded, key);
			check(entry["user_present"].toBool() && entry["effective"] == key.explicitValue,
			      "explicit codec settings persist after closing and reopening the profile");
		}
		finish();
	} catch (const std::exception &e) {
		finish(QString::fromUtf8(e.what()));
	}
}
void event(obs_frontend_event type, void *)
{
	if (type == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		shuttingDown = true;
		done = true;
		QSaveFile file(root + "/shutdown.json");
		if (file.open(QIODevice::WriteOnly)) {
			file.write(QJsonDocument(QJsonObject{{"profile_operation_on_stack", profileOperation},
							     {"profile_or_UI_restored", false},
							     {"runtime_result_unchanged", true}})
					   .toJson());
			file.commit();
		}
	} else if (type == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		QTimer::singleShot(1500, qApp, [] {
			if (!done) {
				run();
			}
		});
	}
}
} // namespace
bool obs_module_load()
{
	root = qEnvironmentVariable("OBS_STUDIO_PROFILE_DEFAULTS_ROOT");
	configRoot = qEnvironmentVariable("OBS_STUDIO_PROFILE_DEFAULTS_CONFIG_ROOT");
	expectation = qEnvironmentVariable("OBS_STUDIO_PROFILE_DEFAULTS_EXPECT", "after");
	if (root.isEmpty() || configRoot.isEmpty() || (expectation != "before" && expectation != "after") ||
	    QFileInfo::exists(root + "/runtime.json") || !QDir().mkpath(root)) {
		return false;
	}
	obs_frontend_add_event_callback(event, nullptr);
	QTimer::singleShot(45000, qApp, [] {
		if (!done) {
			finish("profile fixture exceeded its 45 second deadline");
		}
	});
	return true;
}
void obs_module_unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
}
