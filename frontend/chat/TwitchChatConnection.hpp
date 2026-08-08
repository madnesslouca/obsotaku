/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnection.hpp"

#include <QPointer>
#include <QSslSocket>

/* Twitch chat over IRC.
 *
 * Joins as the account itself when a stored token carries the chat scopes, so
 * the streamer can reply from the dock, and falls back to an anonymous
 * justinfan login otherwise — reading always works, sending needs the token. */
class TwitchChatConnection : public ChatConnection {
	Q_OBJECT

public:
	using ChatConnection::ChatConnection;

	void Start() override;
	void Stop() override;
	bool CanSend() const override;
	bool SendText(const QString &text, QString &error) override;

private:
	void LoadTokenAndOpen();
	void OpenSocket();
	void OnConnected();
	void OnReadyRead();
	void OnError();
	void OnDisconnected();
	void ParseIrcLine(const QString &line);

	QPointer<QSslSocket> socket;
	QString oauthToken;
	QString ircNick;
	bool authenticated = false;
};
