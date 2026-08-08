/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnection.hpp"

#include <QByteArray>
#include <QPointer>
#include <QSslSocket>

/* Kick chat over the Pusher WebSocket the site itself uses.
 *
 * Read-only: the socket carries no way to post, and the official API path for
 * sending is not wired up here. The chatroom id has to be resolved first from
 * an endpoint that sits behind Cloudflare. */
class KickChatConnection : public ChatConnection {
	Q_OBJECT

public:
	using ChatConnection::ChatConnection;

	void Start() override;
	void Stop() override;

private:
	void ResolveChatroom();
	void OpenSocket();
	void OnConnected();
	void OnReadyRead();
	void OnError();
	void OnDisconnected();
	bool ProcessHandshake();
	void ProcessFrames();
	void SendFrame(quint8 opcode, const QByteArray &payload);
	void HandlePayload(const QByteArray &payload);

	QPointer<QSslSocket> socket;
	QByteArray buffer;
	QByteArray fragment;
	QByteArray handshakeKey;
	QString chatroomId;
	quint8 fragmentOpcode = 0;
	bool handshakeComplete = false;
};
