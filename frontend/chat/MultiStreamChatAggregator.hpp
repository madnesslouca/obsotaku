/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnection.hpp"
#include "TwitchEventSub.hpp"

#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <functional>

#include <memory>
#include <vector>

/* Holds one live chat connection per destination and forwards what they say.
 *
 * Every method addresses a channel by its id rather than by platform, so two
 * accounts on the same platform are two independent connections. */
class MultiStreamChatAggregator : public QObject {
	Q_OBJECT

public:
	using ConnectionFactory = std::function<std::unique_ptr<ChatConnection>(const ChatChannelRef &,
										QNetworkAccessManager *, QObject *)>;
	explicit MultiStreamChatAggregator(QObject *parent = nullptr, ConnectionFactory factory = {});
	~MultiStreamChatAggregator() override;

	/* Replaces the live set: drops connections no longer listed, keeps the
	 * ones whose target did not change, and starts the new ones. */
	void SetChannels(const std::vector<ChatChannelRef> &channels);
	/* Stops timers, sockets and new work while keeping the QObject instances
	 * alive until background token tasks have drained during shutdown. */
	void StopAll();
	void DisconnectAll();

	bool IsConnected(const QString &channelId) const;
	bool CanSend(const QString &channelId) const;
	/* False when the message could not be handed to the platform at all. */
	bool SendText(const QString &channelId, const QString &text, const QString &requestId, QString &error);

	/* Whether this platform has a send path at all, regardless of any
	 * connection being up. */
	static bool PlatformCanSend(StreamPlatform platform);

signals:
	void sendFinished(const ChatSendResult &result);
	void broadcastsAvailable(const QString &channelId, const QList<ChatBroadcast> &broadcasts);
	void messageRemoved(const QString &channelId, const QString &messageId, const QString &senderId);
	void messageReceived(const ChatMessage &message);
	void messageUpdated(const ChatMessage &message);
	void activityStatusChanged(const QString &channelId, ChatConnectionState state, const QString &detail);
	void statusChanged(const QString &channelId, StreamPlatform platform, ChatConnectionState state,
			   const QString &detail);

private:
	void ForwardMessage(const ChatMessage &message);
	void RefreshActivity(const std::vector<ChatChannelRef> &channels);
	ChatConnection *FindConnection(const QString &channelId) const;
	static std::unique_ptr<ChatConnection> MakeConnection(const ChatChannelRef &channel,
							      QNetworkAccessManager *netManager, QObject *parent);

	std::vector<ChatChannelRef> channels;
	std::vector<std::unique_ptr<TwitchEventSub>> activity;
	ConnectionFactory factory;
	QSet<QString> seenIds;
	QQueue<QString> seenOrder;
	QNetworkAccessManager *netManager = nullptr;
	std::vector<std::unique_ptr<ChatConnection>> connections;
};
