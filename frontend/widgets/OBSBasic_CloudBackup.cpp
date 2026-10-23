/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OBSBasic.hpp"

#include <oauth/GoogleDriveBackupClient.hpp>
#include <utility/CloudBackupArchive.hpp>
#include <utility/MultistreamTaskPool.hpp>

#include <OBSApp.hpp>

#include <util/base.h>
#include <util/config-file.h>

#include <QDateTime>
#include <QPointer>
#include <QTimer>

namespace {
constexpr const char *CONFIG_SECTION = "GoogleDriveBackup";
}

void OBSBasic::InitializeGoogleDriveBackupScheduler()
{
	googleDriveBackupTimer.setInterval(15 * 60 * 1000);
	googleDriveBackupTimer.setSingleShot(false);
	connect(&googleDriveBackupTimer, &QTimer::timeout, this, &OBSBasic::RunAutomaticGoogleDriveBackup);
	googleDriveBackupTimer.start();
	QTimer::singleShot(45 * 1000, this, &OBSBasic::RunAutomaticGoogleDriveBackup);
}

void OBSBasic::RunAutomaticGoogleDriveBackup()
{
	config_t *config = App()->GetUserConfig();
	if (googleDriveBackupRunning || !config_get_bool(config, CONFIG_SECTION, "AutoEnabled") ||
	    StreamingActive() || RecordingActive())
		return;
	const int intervalDays = std::max(1, static_cast<int>(config_get_int(config, CONFIG_SECTION, "IntervalDays")));
	const int64_t lastBackup = config_get_int(config, CONFIG_SECTION, "LastBackupUnix");
	const int64_t now = QDateTime::currentSecsSinceEpoch();
	if (lastBackup > 0 && now - lastBackup < static_cast<int64_t>(intervalDays) * 24 * 60 * 60)
		return;

	SaveProjectNow();
	config_save_safe(App()->GetAppConfig(), "tmp", nullptr);
	config_save_safe(config, "tmp", nullptr);
	CloudBackupOptions options;
	options.generalSettings = !config_has_user_value(config, CONFIG_SECTION, "IncludeGeneral") ||
				  config_get_bool(config, CONFIG_SECTION, "IncludeGeneral");
	options.profiles = !config_has_user_value(config, CONFIG_SECTION, "IncludeProfiles") ||
			   config_get_bool(config, CONFIG_SECTION, "IncludeProfiles");
	options.scenes = !config_has_user_value(config, CONFIG_SECTION, "IncludeScenes") ||
			 config_get_bool(config, CONFIG_SECTION, "IncludeScenes");
	options.pluginSettings = !config_has_user_value(config, CONFIG_SECTION, "IncludePlugins") ||
				 config_get_bool(config, CONFIG_SECTION, "IncludePlugins");
	/* A passphrase is deliberately never stored. Automatic backups therefore
	 * remove secrets; users can create an encrypted credential backup manually. */
	options.includeSecrets = false;
	const int retention = std::max(1, static_cast<int>(config_get_int(config, CONFIG_SECTION, "Retention")));
	googleDriveBackupRunning = true;
	const auto registration = GoogleDriveBackupClient::Registration();
	const auto metadataCurrent = GoogleDriveBackupClient::CaptureConfigurationGuard(GoogleDriveBackupClient::ConfigurationPart::All);
	const auto sessionRevision = GoogleDriveBackupClient::SessionRevision();
	const OAuthHttpClient::Cancellation cancel{{}, MultistreamTaskPool().StopToken()};
	auto request = std::make_shared<SecureTokenStore::RequestContext>();
	QPointer<OBSBasic> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	const bool accepted = MultistreamTaskPool().start([guard, dispatcher, registration, request, options, retention, cancel, metadataCurrent, sessionRevision]() {
		QByteArray archive;
		CloudBackupSummary summary;
		GoogleDriveBackupItem uploaded;
		QString error;
		GoogleDriveBackupClient::SessionInfo session;
		const bool connected = GoogleDriveBackupClient::HasConnection(error, request, &session);
		const bool skipped = !connected && error.isEmpty();
		bool success = connected && session.epoch == sessionRevision && !cancel.IsCancelled();
		if (connected && session.epoch != sessionRevision)
			error = QObject::tr("A conta mudou antes de iniciar o backup automático.");
		if (success) success = CloudBackupArchive::Create(options, archive, summary, error);
		if (success)
			success = GoogleDriveBackupClient::Upload(archive, summary.computerName, summary.obsVersion,
							  summary.createdUtc, false, uploaded, error, registration, cancel, session);
		if (success) {
			QString pruneError;
			if (!GoogleDriveBackupClient::Prune(retention, pruneError, registration, cancel, session))
				blog(LOG_WARNING, "Automatic cloud backup retention failed.");
		}
		QMetaObject::invokeMethod(dispatcher, [guard, success, skipped, metadataCurrent, session]() {
			if (!guard)
				return;
			guard->googleDriveBackupRunning = false;
			if (success && metadataCurrent() && GoogleDriveBackupClient::SessionCurrent(session)) {
				config_t *config = App()->GetUserConfig();
				const char *raw = config_get_string(config, CONFIG_SECTION, "LastBackupUnix");
				const std::optional<std::string> previous = config_has_user_value(config, CONFIG_SECTION, "LastBackupUnix")
					? std::optional<std::string>(raw ? raw : "") : std::nullopt;
				config_set_int(config, CONFIG_SECTION, "LastBackupUnix", QDateTime::currentSecsSinceEpoch());
				if (config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS)
					blog(LOG_INFO, "Automatic Google Drive backup completed");
				else {
					if (previous) config_set_string(config, CONFIG_SECTION, "LastBackupUnix", previous->c_str());
					else config_remove_value(config, CONFIG_SECTION, "LastBackupUnix");
					blog(LOG_WARNING, "Automatic cloud backup timestamp was not saved.");
				}
			} else if (success) {
				blog(LOG_WARNING, "Automatic cloud backup completed for an earlier context; current timestamp unchanged.");
			} else if (!skipped) {
				blog(LOG_WARNING, "Automatic Google Drive backup failed.");
			}
		}, Qt::QueuedConnection);
	});
	if (!accepted)
		googleDriveBackupRunning = false;
}
