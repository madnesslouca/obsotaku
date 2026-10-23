/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "OAuthTokenSet.hpp"
#include "OAuthHttpClient.hpp"
#include "PlatformOAuthClient.hpp"
#include <utility/SecureTokenStore.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <vector>
#include <functional>

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
	/* Nonsecret process-local identity. A refresh preserves the session; a new
	 * authorization, deletion or observed external RAW change replaces it. */
	struct SessionInfo {
		uint64_t epoch = 0;
		bool connected = false;
	};
	static bool SessionCurrent(const SessionInfo &session);
	static uint64_t SessionRevision();
	class CredentialTransaction {
	public:
		~CredentialTransaction();
		bool Remove(QString &error);
		bool Store(const OAuthTokenSet &tokens, QString &error);
		bool Rollback(QString &error);
		SessionInfo Commit();
		CredentialTransaction(const CredentialTransaction &) = delete;
		CredentialTransaction &operator=(const CredentialTransaction &) = delete;
	private:
		friend class GoogleDriveBackupClient;
		struct Impl;
		explicit CredentialTransaction(std::unique_ptr<Impl> state);
		std::unique_ptr<Impl> state;
	};
	/* Begin, Remove, decision wait, Rollback/Commit and destruction all remain
	 * on one worker. The namespace lock is retained through the GUI decision. */
	static std::unique_ptr<CredentialTransaction> BeginCredentialTransaction(const SessionInfo &expected, QString &error,
		std::shared_ptr<SecureTokenStore::RequestContext> request);
	/* Capture/invoke only on GUI. The closure holds no config pointer or secret. */
	enum class ConfigurationPart { Connection, Settings, All };
	static std::function<bool()> CaptureConfigurationGuard(ConfigurationPart part = ConfigurationPart::Connection);
	static OAuthClientRegistration Registration();
	static bool RegistrationReady();
	/* Worker-only credential I/O. Snapshot Registration() on GUI and pass it
	 * explicitly to remote operations; workers never read/write App config. */
	static bool HasConnection(QString &error, std::shared_ptr<SecureTokenStore::RequestContext> request = {},
		SessionInfo *session = nullptr);
	static bool StoreTokens(const OAuthTokenSet &tokens, QString &error);
	static bool Disconnect(QString &error, std::shared_ptr<SecureTokenStore::RequestContext> request = {});
	static bool ValidateSession(const SessionInfo &expected, QString &error,
		std::shared_ptr<SecureTokenStore::RequestContext> request = {});
	static bool AuthorizedAccountName(const OAuthTokenSet &tokens, QString &name, QString &error,
		const OAuthHttpClient::Cancellation &cancel = {});
	static bool AccountName(QString &name, QString &error, const OAuthClientRegistration &registration,
			 const OAuthHttpClient::Cancellation &cancel = {},
		const std::optional<SessionInfo> &expected = {});

	static bool List(std::vector<GoogleDriveBackupItem> &items, QString &error,
			 const OAuthClientRegistration &registration,
			 const OAuthHttpClient::Cancellation &cancel = {},
		const std::optional<SessionInfo> &expected = {});
	static bool Upload(const QByteArray &archive, const QString &computerName, const QString &obsVersion,
			   const QDateTime &createdUtc, bool encrypted, GoogleDriveBackupItem &item, QString &error,
			   const OAuthClientRegistration &registration,
			 const OAuthHttpClient::Cancellation &cancel = {},
		const std::optional<SessionInfo> &expected = {});
	static bool Download(const QString &fileId, QByteArray &archive, QString &error,
			     const OAuthClientRegistration &registration,
			 const OAuthHttpClient::Cancellation &cancel = {},
		const std::optional<SessionInfo> &expected = {});
	static bool Remove(const QString &fileId, QString &error, const OAuthClientRegistration &registration,
			 const OAuthHttpClient::Cancellation &cancel = {},
		const std::optional<SessionInfo> &expected = {});
	static bool Prune(int retainCount, QString &error, const OAuthClientRegistration &registration,
			 const OAuthHttpClient::Cancellation &cancel = {},
		const std::optional<SessionInfo> &expected = {});

	static constexpr std::string_view Scope = "https://www.googleapis.com/auth/drive.appdata";

private:
	static bool AccessToken(std::string &token, QString &error, const OAuthClientRegistration &registration,
		const std::optional<SessionInfo> &expected, SessionInfo &effective);
};
