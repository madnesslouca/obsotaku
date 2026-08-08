/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnection.hpp"

#include <functional>

/* YouTube live chat over the authenticated API.
 *
 * There is no socket: the chat is polled at the interval the API asks for, and
 * the live chat id has to be found first from the account's active broadcast,
 * which only exists while the channel is actually live. */
class YouTubeChatConnection : public ChatConnection {
	Q_OBJECT

public:
	using ChatConnection::ChatConnection;

	void Start() override;
	void Stop() override;
	bool CanSend() const override;
	bool SendText(const QString &text, QString &error) override;

private:
	void ResolveLiveChat();
	void Poll();
	void WithAccessToken(std::function<void(const QString &)> continuation);
	void ScheduleRetry(int milliseconds);

	QTimer *pollTimer = nullptr;
	QString liveChatId;
	QString nextPageToken;
	QString accessToken;
	qint64 accessTokenExpiresAt = 0;
	/* The first poll returns the backlog; emitting it would replay old
	 * messages every time the dock is reopened. */
	bool primed = false;
	bool requestInFlight = false;
};
