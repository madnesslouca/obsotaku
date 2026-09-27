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
	    !GoogleDriveBackupClient::HasConnection() || StreamingActive() || RecordingActive())
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
	QPointer<OBSBasic> guard(this);
	MultistreamTaskPool().start([guard, options, retention]() {
		QByteArray archive;
		CloudBackupSummary summary;
		GoogleDriveBackupItem uploaded;
		QString error;
		bool success = CloudBackupArchive::Create(options, archive, summary, error);
		if (success)
			success = GoogleDriveBackupClient::Upload(archive, summary.computerName, summary.obsVersion,
							  summary.createdUtc, false, uploaded, error);
		if (success) {
			QString pruneError;
			if (!GoogleDriveBackupClient::Prune(retention, pruneError))
				blog(LOG_WARNING, "Automatic cloud backup retention failed: %s", QT_TO_UTF8(pruneError));
		}
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, success, error]() {
			if (!guard)
				return;
			guard->googleDriveBackupRunning = false;
			if (success) {
				config_set_int(App()->GetUserConfig(), CONFIG_SECTION, "LastBackupUnix",
					       QDateTime::currentSecsSinceEpoch());
				config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
				blog(LOG_INFO, "Automatic Google Drive backup completed");
			} else {
				blog(LOG_WARNING, "Automatic Google Drive backup failed: %s", QT_TO_UTF8(error));
			}
		}, Qt::QueuedConnection);
	});
}
