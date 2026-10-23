/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <functional>

/* Capture on the GUI/startup thread before opening a restore modal or queuing
 * work. The transport archive never supplies local absolute paths. Copies of
 * this value are worker inputs; no archive operation re-reads App() roots. */
struct CloudBackupRoots {
	QString configRoot;
	QString globalConfigPath;
	QString profilesRoot;
	QString scenesRoot;
	QString pluginConfigRoot;
	QString pluginManagerRoot;
	QString pendingRoot;
	QString safetyRoot;
	QString coordinationRoot;
	QString obsVersion;
	QString identity;
};

struct CloudBackupOptions {
	bool generalSettings = true;
	bool profiles = true;
	bool scenes = true;
	bool pluginSettings = true;
	bool includeSecrets = false;
	bool restoreCredentials = false;
	bool preserveLocalDevices = true;
	QString passphrase;
	QList<QPair<QString, QString>> pathMappings;
};

struct CloudBackupSummary {
	int formatVersion = 0;
	QString obsVersion;
	QString computerName;
	QDateTime createdUtc;
	bool encrypted = false;
	int fileCount = 0;
	qint64 unpackedBytes = 0;
};

class CloudBackupArchive {
public:
	static bool CaptureRoots(CloudBackupRoots &roots, QString &error);
	/* GUI/startup only; checks App roots against the captured identity. */
	static bool RootsCurrent(const CloudBackupRoots &roots, QString &error);
	static bool Create(const CloudBackupRoots &roots, const CloudBackupOptions &options, QByteArray &archive, CloudBackupSummary &summary,
			   QString &error, const std::function<bool()> &cancelled = {});
	static bool Inspect(const QByteArray &archive, const QString &passphrase, CloudBackupSummary &summary,
			    QString &error);
	static bool StageRestore(const CloudBackupRoots &roots, const QByteArray &archive, const CloudBackupOptions &options, QString &safetyBackup,
				 QString &error, const std::function<bool()> &cancelled = {});
	/* Called after OBS has resolved its configuration/profile/scene roots but
	 * before user.ini and the active collection are loaded. */
	static bool ApplyPendingRestore(const CloudBackupRoots &roots, QString &error);
	static bool HasPendingRestore(const CloudBackupRoots &roots);

	static bool EncryptionSupported();
	static QString LocalBackupDirectory(const CloudBackupRoots &roots);
};
