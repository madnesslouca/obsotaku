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

struct ChatMessage {
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	QString channelName;
	QString senderName;
	QString messageText;
	QString userColor;
	QString timestamp;
	bool isModerator = false;
	bool isSubscriber = false;
	bool isVip = false;
};

class MultiStreamChatAggregator : public QObject {
	Q_OBJECT

public:
	explicit MultiStreamChatAggregator(QObject *parent = nullptr);
	~MultiStreamChatAggregator() override;

	/* channelNameOrId is the IRC channel for Twitch, the channel slug for
	 * Kick and the connected account id for YouTube. */
	void ConnectPlatform(StreamPlatform platform, const QString &channelNameOrId);
	void DisconnectPlatform(StreamPlatform platform);
	void DisconnectAll();

	bool IsConnected(StreamPlatform platform) const;

signals:
	void messageReceived(const ChatMessage &message);
	void statusChanged(StreamPlatform platform, ChatConnectionState state, const QString &detail);

private:
	// Twitch IRC
	void StartTwitch();
	void OnTwitchConnected();
	void OnTwitchReadyRead();
	void OnTwitchError(QAbstractSocket::SocketError socketError);
	void OnTwitchDisconnected();
	void ParseTwitchIrcLine(const QString &line);

	// Kick WebSocket (Pusher)
	void StartKick();
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
	void ResolveYouTubeLiveChat();
	void PollYouTubeChat();
	void WithYouTubeAccessToken(std::function<void(const QString &)> continuation);
	void ScheduleYouTubeRetry(int milliseconds);

	void EmitStatus(StreamPlatform platform, ChatConnectionState state, const QString &detail = {});
	void ScheduleReconnect(StreamPlatform platform);

	QNetworkAccessManager *netManager = nullptr;

	// Twitch
	QPointer<QSslSocket> twitchSocket;
	QString twitchChannel;
	QTimer *twitchReconnectTimer = nullptr;
	int twitchReconnectDelayMs = 5000;
	bool twitchConnected = false;

	// Kick
	QPointer<QSslSocket> kickSocket;
	QByteArray kickBuffer;
	QByteArray kickFragment;
	QByteArray kickHandshakeKey;
	QString kickChatroomId;
	QString kickChannel;
	QTimer *kickReconnectTimer = nullptr;
	int kickReconnectDelayMs = 5000;
	quint8 kickFragmentOpcode = 0;
	bool kickHandshakeComplete = false;
	bool kickConnected = false;

	// YouTube
	QTimer *ytPollTimer = nullptr;
	QString ytAccountId;
	QString ytLiveChatId;
	QString ytNextPageToken;
	QString ytAccessToken;
	qint64 ytAccessTokenExpiresAt = 0;
	bool ytPrimed = false;
	bool ytConnected = false;
	bool ytRequestInFlight = false;
};
