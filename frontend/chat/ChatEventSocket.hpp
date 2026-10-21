/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once
#include <QObject>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

// Receive-only WSS client for Twitch EventSub. Only protocol pong/close frames are sent.
class ChatEventSocket : public QObject {
	Q_OBJECT
public:
	explicit ChatEventSocket(QObject *parent = nullptr);
	void Open(const QUrl &url);
	void Stop();
	static bool AllowedUrl(const QUrl &url);
signals:
	void textReceived(const QByteArray &text);
	void closed();

private:
	friend class ChatIntegrationTest;
	void Read();
	bool Handshake();
	void Frames();
	void Fail();
	void Control(quint8 opcode, const QByteArray &payload);
	QSslSocket socket;
	QTimer deadline;
	QUrl url;
	QByteArray key, buffer, fragments;
	bool upgraded = false;
	bool fragmented = false;
	bool running = false;
};
