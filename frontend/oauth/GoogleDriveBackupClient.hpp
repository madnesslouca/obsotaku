/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "OAuthTokenSet.hpp"
#include "PlatformOAuthClient.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <vector>

struct GoogleDriveBackupItem {
	QString id;
	QString name;
	QString computerName;
	QString obsVersion;
	QDateTime createdUtc;
	qint64 size = 0;
	bool encrypted = false;
};

class GoogleDriveBackupClient {
public:
	static OAuthClientRegistration Registration();
	static bool RegistrationReady();
	static bool HasConnection();
	static bool StoreTokens(const OAuthTokenSet &tokens, QString &error);
	static bool Disconnect(QString &error);
	static bool AccountName(QString &name, QString &error);

	static bool List(std::vector<GoogleDriveBackupItem> &items, QString &error);
	static bool Upload(const QByteArray &archive, const QString &computerName, const QString &obsVersion,
			   const QDateTime &createdUtc, bool encrypted, GoogleDriveBackupItem &item, QString &error);
	static bool Download(const QString &fileId, QByteArray &archive, QString &error);
	static bool Remove(const QString &fileId, QString &error);
	static bool Prune(int retainCount, QString &error);

	static constexpr std::string_view Scope = "https://www.googleapis.com/auth/drive.appdata";

private:
	static bool AccessToken(std::string &token, QString &error);
};
