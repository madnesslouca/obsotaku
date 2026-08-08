/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/StreamPlatform.hpp>

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

enum class ChatConnectionState {
	Disconnected,
	Connecting,
	Connected,
	Failed,
	MissingCredential,
	WaitingForBroadcast,
	/* The platform has no public chat API we can read. */
	Unsupported,
};

enum class ChatMessageKind {
	Normal,
	SuperChat,
	Membership,
	System,
};

/* One multistream destination the dock wants chat for. address is the IRC
 * login / Kick slug / YouTube account id depending on the platform. */
struct ChatChannelRef {
	QString channelId;
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	QString address;
	QString displayName;
	QString accountId;
};

struct ChatMessage {
	QString channelId;
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	QString channelName;
	QString senderName;
	QString messageText;
	QString userColor;
	QString timestamp;
	bool isModerator = false;
	bool isSubscriber = false;
	bool isVip = false;
	bool isBroadcaster = false;
	/* Short role labels shown next to the nick (MOD, SUB, VIP, ...). */
	QStringList roleBadges;
	ChatMessageKind kind = ChatMessageKind::Normal;
	QString paidAmount;
	QString paidCurrency;
};

QString ChatCurrentTimestamp();
QStringList BuildRoleBadges(const ChatMessage &message);

/* One live chat connection, for one destination.
 *
 * Everything that differs between platforms lives in a subclass; the shared
 * parts are the channel it belongs to, the backoff timer, and the way status
 * and messages reach the dock. One instance per destination, so two accounts on
 * the same platform are two connections that know nothing about each other. */
class ChatConnection : public QObject {
	Q_OBJECT

public:
	ChatConnection(ChatChannelRef channel, QNetworkAccessManager *netManager, QObject *parent = nullptr);
	~ChatConnection() override = default;

	const ChatChannelRef &Channel() const { return channel; }
	/* True when a new reference describes the same live connection, so the
	 * dock can rebuild its list without dropping connections that did not
	 * change. */
	bool SameTargetAs(const ChatChannelRef &other) const;

	virtual void Start() = 0;
	virtual void Stop() = 0;

	bool IsConnected() const { return connected; }
	virtual bool CanSend() const { return false; }
	/* False when the message could not even be handed to the platform. A
	 * failure after that arrives on statusChanged. */
	virtual bool SendText(const QString &text, QString &error);

signals:
	void messageReceived(const ChatMessage &message);
	void statusChanged(const QString &channelId, StreamPlatform platform, ChatConnectionState state,
			   const QString &detail);

protected:
	void EmitStatus(ChatConnectionState state, const QString &detail = {});
	/* Reconnects with an exponential backoff, capped, and only while the
	 * connection still has a target. */
	void ScheduleReconnect();
	void ResetReconnectDelay();
	void StopReconnect();
	/* A message with the fields every platform fills the same way. */
	ChatMessage NewMessage() const;
	QString DisplayName() const;

	ChatChannelRef channel;
	QNetworkAccessManager *netManager = nullptr;
	QTimer *reconnectTimer = nullptr;
	int reconnectDelayMs = 5000;
	bool connected = false;
};
