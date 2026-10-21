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
#include <QJsonDocument>
#include <QJsonArray>
#include <utility>

#include "moc_MultiStreamChatAggregator.cpp"

MultiStreamChatAggregator::MultiStreamChatAggregator(QObject *parent, ConnectionFactory connectionFactory)
	: QObject(parent),
	  factory(connectionFactory ? std::move(connectionFactory) : MakeConnection)
{
	netManager = new QNetworkAccessManager(this);
}

MultiStreamChatAggregator::~MultiStreamChatAggregator()
{
	DisconnectAll();
}

std::unique_ptr<ChatConnection> MultiStreamChatAggregator::MakeConnection(const ChatChannelRef &channel,
									  QNetworkAccessManager *manager,
									  QObject *parent)
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
		if (channel.address.trimmed().isEmpty()) {
			continue;
		}
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
		if (connection) {
			connection->Stop();
		}
	}
	connections.clear();
	connections = std::move(kept);

	for (const ChatChannelRef *channel : toOpen) {
		auto connection = factory(*channel, netManager, this);
		if (!connection) {
			continue;
		}
		connect(connection.get(), &ChatConnection::messageReceived, this,
			&MultiStreamChatAggregator::ForwardMessage);
		connect(connection.get(), &ChatConnection::sendFinished, this,
			&MultiStreamChatAggregator::sendFinished);
		connect(connection.get(), &ChatConnection::broadcastsAvailable, this,
			&MultiStreamChatAggregator::broadcastsAvailable);
		connect(connection.get(), &ChatConnection::messageRemoved, this,
			&MultiStreamChatAggregator::messageRemoved);
		connect(connection.get(), &ChatConnection::statusChanged, this,
			&MultiStreamChatAggregator::statusChanged);
		connections.push_back(std::move(connection));
		connections.back()->Start();
	}
	this->channels = channels;
	RefreshActivity(channels);
}

void MultiStreamChatAggregator::DisconnectAll()
{
	StopAll();
	connections.clear();
	activity.clear();
	channels.clear();
}

void MultiStreamChatAggregator::StopAll()
{
	for (auto &events : activity) {
		events->Stop();
	}
	for (auto &connection : connections) {
		if (connection) {
			connection->Stop();
		}
	}
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

bool MultiStreamChatAggregator::SendText(const QString &channelId, const QString &text, const QString &requestId,
					 QString &error)
{
	const QString trimmed = text.trimmed();
	if (trimmed.isEmpty() || requestId.isEmpty() || trimmed.contains(QChar(0)) || trimmed.contains('\r') ||
	    trimmed.contains('\n')) {
		error = "empty";
		return false;
	}

	ChatConnection *connection = FindConnection(channelId);
	if (!connection) {
		error = "unsupported";
		return false;
	}
	return connection->SendText(trimmed, requestId, error);
}

bool MultiStreamChatAggregator::PlatformCanSend(StreamPlatform platform)
{
	return platform == StreamPlatform::Twitch || platform == StreamPlatform::YouTube ||
	       platform == StreamPlatform::Kick;
}

void MultiStreamChatAggregator::ForwardMessage(const ChatMessage &message)
{
	if (!message.id.isEmpty()) {
		const QString key = QString::fromUtf8(
			QJsonDocument(QJsonArray{message.channelId, message.id}).toJson(QJsonDocument::Compact));
		if (seenIds.contains(key)) {
			if (!message.emotes.isEmpty()) {
				emit messageUpdated(message);
			}
			return;
		}
		seenIds.insert(key);
		seenOrder.enqueue(key);
		while (seenOrder.size() > 3000) {
			seenIds.remove(seenOrder.dequeue());
		}
	}
	emit messageReceived(message);
}

void MultiStreamChatAggregator::RefreshActivity(const std::vector<ChatChannelRef> &targets)
{
	QSet<QString> wanted;
	for (const auto &target : targets) {
		if (target.platform == StreamPlatform::Twitch && !target.accountId.isEmpty()) {
			wanted.insert(target.accountId);
		}
	}
	for (auto it = activity.begin(); it != activity.end();) {
		if (!wanted.contains((*it)->Channel().accountId)) {
			(*it)->Stop();
			it = activity.erase(it);
		} else {
			++it;
		}
	}
	for (const auto &account : wanted) {
		const auto existing = std::find_if(activity.begin(), activity.end(), [&](const auto &item) {
			return item->Channel().accountId == account;
		});
		if (existing != activity.end()) {
			if ((*existing)->NeedsCredentials()) {
				(*existing)->Start();
			}
			continue;
		}
		ChatChannelRef ref;
		ref.accountId = account;
		ref.address = account;
		ref.platform = StreamPlatform::Twitch;
		auto events = std::make_unique<TwitchEventSub>(ref, netManager, this);
		connect(events.get(), &ChatConnection::messageReceived, this,
			[this, account](const ChatMessage &event) {
				for (const auto &target : channels) {
					if (target.platform != StreamPlatform::Twitch || target.accountId != account) {
						continue;
					}
					auto message = event;
					message.channelId = target.channelId;
					message.channelName = target.displayName;
					ForwardMessage(message);
				}
			});
		connect(events.get(), &ChatConnection::statusChanged, this,
			[this, account](const QString &, StreamPlatform, ChatConnectionState state,
					const QString &detail) {
				for (const auto &target : channels) {
					if (target.platform == StreamPlatform::Twitch && target.accountId == account) {
						emit activityStatusChanged(target.channelId, state, detail);
					}
				}
			});
		activity.push_back(std::move(events));
		activity.back()->Start();
	}
}
