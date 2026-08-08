/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultiStreamChatAggregator.hpp"

#include "KickChatConnection.hpp"
#include "TwitchChatConnection.hpp"
#include "YouTubeChatConnection.hpp"

#include <algorithm>
#include <utility>

#include "moc_MultiStreamChatAggregator.cpp"

MultiStreamChatAggregator::MultiStreamChatAggregator(QObject *parent) : QObject(parent)
{
	netManager = new QNetworkAccessManager(this);
}

MultiStreamChatAggregator::~MultiStreamChatAggregator()
{
	DisconnectAll();
}

std::unique_ptr<ChatConnection> MultiStreamChatAggregator::MakeConnection(const ChatChannelRef &channel,
									 QNetworkAccessManager *manager, QObject *parent)
{
	switch (channel.platform) {
	case StreamPlatform::Twitch:
		return std::make_unique<TwitchChatConnection>(channel, manager, parent);
	case StreamPlatform::Kick:
		return std::make_unique<KickChatConnection>(channel, manager, parent);
	case StreamPlatform::YouTube:
		return std::make_unique<YouTubeChatConnection>(channel, manager, parent);
	default:
		return nullptr;
	}
}

ChatConnection *MultiStreamChatAggregator::FindConnection(const QString &channelId) const
{
	const auto item = std::find_if(connections.begin(), connections.end(),
				       [&](const std::unique_ptr<ChatConnection> &connection) {
					       return connection->Channel().channelId == channelId;
				       });
	return item != connections.end() ? item->get() : nullptr;
}

void MultiStreamChatAggregator::SetChannels(const std::vector<ChatChannelRef> &channels)
{
	std::vector<std::unique_ptr<ChatConnection>> kept;
	std::vector<const ChatChannelRef *> toOpen;

	for (const auto &channel : channels) {
		if (channel.address.trimmed().isEmpty())
			continue;
		if (!GetStreamPlatformInfo(channel.platform).supportsChat) {
			emit statusChanged(channel.channelId, channel.platform, ChatConnectionState::Unsupported, {});
			continue;
		}

		/* A connection whose target is unchanged is carried over as it is:
		 * rebuilding it would drop a working socket and replay the reconnect
		 * backoff every time the dock refreshes its list. */
		auto existing = std::find_if(connections.begin(), connections.end(),
					     [&](const std::unique_ptr<ChatConnection> &connection) {
						     return connection && connection->SameTargetAs(channel);
					     });
		if (existing != connections.end()) {
			kept.push_back(std::move(*existing));
			continue;
		}
		toOpen.push_back(&channel);
	}

	/* Whatever was not carried over is no longer wanted. */
	for (auto &connection : connections) {
		if (connection)
			connection->Stop();
	}
	connections.clear();
	connections = std::move(kept);

	for (const ChatChannelRef *channel : toOpen) {
		auto connection = MakeConnection(*channel, netManager, this);
		if (!connection)
			continue;
		connect(connection.get(), &ChatConnection::messageReceived, this,
			&MultiStreamChatAggregator::messageReceived);
		connect(connection.get(), &ChatConnection::statusChanged, this,
			&MultiStreamChatAggregator::statusChanged);
		connection->Start();
		connections.push_back(std::move(connection));
	}
}

void MultiStreamChatAggregator::DisconnectAll()
{
	for (auto &connection : connections) {
		if (connection)
			connection->Stop();
	}
	connections.clear();
}

bool MultiStreamChatAggregator::IsConnected(const QString &channelId) const
{
	const ChatConnection *connection = FindConnection(channelId);
	return connection && connection->IsConnected();
}

bool MultiStreamChatAggregator::CanSend(const QString &channelId) const
{
	const ChatConnection *connection = FindConnection(channelId);
	return connection && connection->CanSend();
}

bool MultiStreamChatAggregator::SendText(const QString &channelId, const QString &text, QString &error)
{
	const QString trimmed = text.trimmed();
	if (trimmed.isEmpty()) {
		error = "empty";
		return false;
	}

	ChatConnection *connection = FindConnection(channelId);
	if (!connection) {
		error = "unsupported";
		return false;
	}
	return connection->SendText(trimmed, error);
}

bool MultiStreamChatAggregator::PlatformCanSend(StreamPlatform platform)
{
	/* Kick's chat arrives over the Pusher socket, which carries no way to
	 * post; the official API path for sending is not wired up here. */
	return platform == StreamPlatform::Twitch || platform == StreamPlatform::YouTube;
}
