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
#include <QPointer>
#include <QSet>
#include <QJsonObject>
#include <QNetworkReply>
#include <functional>

enum class ChatConnectionState {
	Disconnected,
	Connecting,
	Connected,
	Failed,
	MissingCredential,
	WaitingForBroadcast,
	SelectBroadcast,
	/* The platform has no public chat API we can read. */
	Unsupported,
};

enum class ChatMessageKind {
	Normal,
	SuperChat,
	Membership,
	Follow,
	Raid,
	ChannelPoints,
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
	QString broadcastId;
};

struct ChatBroadcast {
	QString id;
	QString title;
	QString liveChatId;
};

// A timeout is ambiguous: never automatically retry a possibly delivered message.
enum class ChatSendState { Delivered, Rejected, Unknown };
struct ChatSendResult {
	QString channelId;
	QString requestId;
	ChatSendState state = ChatSendState::Unknown;
	QString providerId;
	QString detail;
};
Q_DECLARE_METATYPE(ChatSendResult)

struct ChatEmote {
	// UTF-16 offsets into messageText, matching QString operations.
	int start = 0;
	int length = 0;
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	QString id;
};

struct ChatMessage {
	QList<ChatEmote> emotes;
	QString eventType;
	QString eventTitle;
	int eventCount = 0;
	QString id;
	QString senderId;
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

Q_DECLARE_METATYPE(ChatMessage)
Q_DECLARE_METATYPE(QList<ChatBroadcast>)

struct ChatAccessToken {
	QString token;
	QString clientId;
	QStringList scopes;
};
using ChatTokenCallback = std::function<void(const ChatAccessToken &)>;
using ChatTokenProvider = std::function<void(StreamPlatform, const QString &, QObject *, ChatTokenCallback)>;
void LoadChatAccessToken(StreamPlatform, const QString &, QObject *, ChatTokenCallback);

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
	ChatConnection(ChatChannelRef channel, QNetworkAccessManager *netManager, QObject *parent = nullptr,
		       ChatTokenProvider tokenProvider = {});
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
	/* Accepted requests always produce one sendFinished result. */
	virtual bool SendText(const QString &text, const QString &requestId, QString &error);

signals:
	void sendFinished(const ChatSendResult &result);
	void broadcastsAvailable(const QString &channelId, const QList<ChatBroadcast> &broadcasts);
	void messageRemoved(const QString &channelId, const QString &messageId, const QString &senderId);
	void messageReceived(const ChatMessage &message);
	void statusChanged(const QString &channelId, StreamPlatform platform, ChatConnectionState state,
			   const QString &detail);

protected:
	void BeginSession();
	void EndSession();
	void WithAccessToken(ChatTokenCallback callback);
	void TrackReply(QNetworkReply *reply);
	void BeginSend(const QString &requestId);
	void FinishSend(const QString &requestId, ChatSendState state, const QString &providerId = {},
			const QString &detail = {});
	void PostMessage(const QString &requestId, const QString &text, const QNetworkRequest &request,
			 const QJsonObject &body);
	quint64 generation = 0;
	bool stopped = true;
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

private:
	ChatTokenProvider tokenProvider;
	QList<QPointer<QNetworkReply>> replies;
	QSet<QString> pendingSends;
};
