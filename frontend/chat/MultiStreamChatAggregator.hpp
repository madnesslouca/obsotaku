/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/StreamPlatform.hpp>

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSslSocket>
#include <QTimer>

#include <functional>
#include <vector>

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

class MultiStreamChatAggregator : public QObject {
	Q_OBJECT

public:
	explicit MultiStreamChatAggregator(QObject *parent = nullptr);
	~MultiStreamChatAggregator() override;

	/* Replaces the live chat set: disconnects anything no longer listed and
	 * connects each new destination. One live connection per platform for
	 * now (first enabled wins); channelId is still tracked on every event. */
	void SetChannels(const std::vector<ChatChannelRef> &channels);
	void DisconnectAll();

	bool IsConnected(StreamPlatform platform) const;
	bool CanSend(StreamPlatform platform) const;
	/* Sends on the live connection for that platform. Returns false when
	 * the path is unavailable (anonymous Twitch, no YT chat id, ...). */
	bool SendText(StreamPlatform platform, const QString &text, QString &error);

signals:
	void messageReceived(const ChatMessage &message);
	void statusChanged(const QString &channelId, StreamPlatform platform, ChatConnectionState state,
			   const QString &detail);

private:
	// Twitch IRC
	void StartTwitch();
	void DisconnectAllTwitch();
	void LoadTwitchAuthAndStart();
	void OpenTwitchSocket();
	void OnTwitchConnected();
	void OnTwitchReadyRead();
	void OnTwitchError(QAbstractSocket::SocketError socketError);
	void OnTwitchDisconnected();
	void ParseTwitchIrcLine(const QString &line);

	// Kick WebSocket (Pusher)
	void StartKick();
	void DisconnectAllKick();
	void ResolveKickChatroom();
	void OpenKickSocket();
	void OnKickConnected();
	void OnKickReadyRead();
	void OnKickError(QAbstractSocket::SocketError socketError);
	void OnKickDisconnected();
	bool ProcessKickHandshake();
	void ProcessKickFrames();
	void SendKickFrame(quint8 opcode, const QByteArray &payload);
	void HandleKickPayload(const QByteArray &payload);

	// YouTube live chat
	void StartYouTube();
	void DisconnectAllYouTube();
	void ResolveYouTubeLiveChat();
	void PollYouTubeChat();
	void WithYouTubeAccessToken(std::function<void(const QString &)> continuation);
	void ScheduleYouTubeRetry(int milliseconds);
	void SendYouTubeText(const QString &text, QString &error);

	void EmitStatus(StreamPlatform platform, ChatConnectionState state, const QString &detail = {});
	void ScheduleReconnect(StreamPlatform platform);
	QString ChannelIdFor(StreamPlatform platform) const;
	QString DisplayNameFor(StreamPlatform platform) const;

	QNetworkAccessManager *netManager = nullptr;

	// Twitch
	QPointer<QSslSocket> twitchSocket;
	QString twitchChannelId;
	QString twitchChannel;
	QString twitchDisplayName;
	QString twitchAccountId;
	QString twitchOauthToken;
	QString twitchIrcNick;
	bool twitchAuthenticated = false;
	QTimer *twitchReconnectTimer = nullptr;
	int twitchReconnectDelayMs = 5000;
	bool twitchConnected = false;

	// Kick
	QPointer<QSslSocket> kickSocket;
	QByteArray kickBuffer;
	QByteArray kickFragment;
	QByteArray kickHandshakeKey;
	QString kickChannelId;
	QString kickChatroomId;
	QString kickChannel;
	QString kickDisplayName;
	QTimer *kickReconnectTimer = nullptr;
	int kickReconnectDelayMs = 5000;
	quint8 kickFragmentOpcode = 0;
	bool kickHandshakeComplete = false;
	bool kickConnected = false;

	// YouTube
	QTimer *ytPollTimer = nullptr;
	QString ytChannelId;
	QString ytAccountId;
	QString ytDisplayName;
	QString ytLiveChatId;
	QString ytNextPageToken;
	QString ytAccessToken;
	qint64 ytAccessTokenExpiresAt = 0;
	bool ytPrimed = false;
	bool ytConnected = false;
	bool ytRequestInFlight = false;
};
