/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once
#include "ChatConnection.hpp"
#include "ChatEventSocket.hpp"
#include <QQueue>

// One authenticated activity session per account, shared by all its outputs.
class TwitchEventSub : public ChatConnection {
	Q_OBJECT
public:
	using ChatConnection::ChatConnection;
	~TwitchEventSub() override { Stop(); }
	void Start() override;
	void Stop() override;
	bool NeedsCredentials() const { return credentialMissing; }

private:
	friend class ChatIntegrationTest;
	void Open(const QUrl &url, bool transfer);
	void Handle(ChatEventSocket *source, const QByteArray &message);
	void Subscribe();
	void Notify(const QJsonObject &root);
	void Retry();
	QPointer<ChatEventSocket> current, pending;
	QTimer *watchdog = nullptr;
	ChatAccessToken access;
	QString sessionId;
	int keepaliveMs = 30000;
	int accepted = 0, answered = 0, requested = 0;
	bool missingPermissions = false;
	bool credentialMissing = false;
	bool denied = false;
	QSet<QString> seen;
	QQueue<QString> seenOrder;
};
