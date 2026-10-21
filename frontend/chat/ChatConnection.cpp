/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatConnection.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>

#include <algorithm>

#include "moc_ChatConnection.cpp"

namespace {
constexpr int MAX_RECONNECT_DELAY_MS = 60000;
constexpr int INITIAL_RECONNECT_DELAY_MS = 5000;
} // namespace

QString ChatCurrentTimestamp()
{
	return QDateTime::currentDateTime().toString(QStringLiteral("hh:mm"));
}

QStringList BuildRoleBadges(const ChatMessage &message)
{
	QStringList badges;
	if (message.isBroadcaster) {
		badges << QStringLiteral("HOST");
	}
	if (message.isModerator) {
		badges << QStringLiteral("MOD");
	}
	if (message.isVip) {
		badges << QStringLiteral("VIP");
	}
	if (message.isSubscriber) {
		badges << QStringLiteral("SUB");
	}
	return badges;
}

ChatConnection::ChatConnection(ChatChannelRef channelRef, QNetworkAccessManager *manager, QObject *parent,
			       ChatTokenProvider provider)
	: QObject(parent),
	  channel(std::move(channelRef)),
	  netManager(manager),
	  tokenProvider(provider ? std::move(provider) : LoadChatAccessToken)
{
	reconnectTimer = new QTimer(this);
	reconnectTimer->setSingleShot(true);
	connect(reconnectTimer, &QTimer::timeout, this, &ChatConnection::Start);
}

bool ChatConnection::SameTargetAs(const ChatChannelRef &other) const
{
	return channel.channelId == other.channelId && channel.platform == other.platform &&
	       channel.address.trimmed().compare(other.address.trimmed(), Qt::CaseInsensitive) == 0 &&
	       channel.accountId == other.accountId && channel.broadcastId == other.broadcastId;
}

bool ChatConnection::SendText(const QString &, const QString &, QString &error)
{
	error = "unsupported";
	return false;
}

void ChatConnection::EmitStatus(ChatConnectionState state, const QString &detail)
{
	emit statusChanged(channel.channelId, channel.platform, state, detail);
}

void ChatConnection::ScheduleReconnect()
{
	if (stopped || channel.address.trimmed().isEmpty()) {
		return;
	}
	reconnectTimer->start(reconnectDelayMs);
	reconnectDelayMs = std::min(reconnectDelayMs * 2, MAX_RECONNECT_DELAY_MS);
}

void ChatConnection::ResetReconnectDelay()
{
	reconnectDelayMs = INITIAL_RECONNECT_DELAY_MS;
}

void ChatConnection::StopReconnect()
{
	reconnectTimer->stop();
}

QString ChatConnection::DisplayName() const
{
	return channel.displayName.isEmpty() ? channel.address : channel.displayName;
}

ChatMessage ChatConnection::NewMessage() const
{
	ChatMessage message;
	message.channelId = channel.channelId;
	message.platform = channel.platform;
	message.channelName = DisplayName();
	message.timestamp = ChatCurrentTimestamp();
	return message;
}

void ChatConnection::BeginSession()
{
	EndSession();
	stopped = false;
}

void ChatConnection::EndSession()
{
	stopped = true;
	connected = false;
	++generation;
	StopReconnect();
	const auto active = replies;
	replies.clear();
	for (const auto &reply : active) {
		if (!reply) {
			continue;
		}
		reply->disconnect(this);
		reply->abort();
		reply->deleteLater();
	}
	const auto pending = pendingSends;
	for (const QString &id : pending) {
		FinishSend(id, ChatSendState::Unknown, {}, QStringLiteral("cancelled"));
	}
}

void ChatConnection::WithAccessToken(ChatTokenCallback callback)
{
	const auto serial = generation;
	QPointer<ChatConnection> guard(this);
	tokenProvider(channel.platform, channel.accountId, this,
		      [guard, serial, callback = std::move(callback)](const ChatAccessToken &token) {
			      if (guard && !guard->stopped && guard->generation == serial) {
				      callback(token);
			      }
		      });
}

void ChatConnection::TrackReply(QNetworkReply *reply)
{
	replies.append(reply);
	connect(reply, &QNetworkReply::finished, this, [this, reply] { replies.removeAll(reply); });
}

void ChatConnection::BeginSend(const QString &id)
{
	pendingSends.insert(id);
}

void ChatConnection::FinishSend(const QString &id, ChatSendState state, const QString &providerId,
				const QString &detail)
{
	if (pendingSends.remove(id)) {
		emit sendFinished({channel.channelId, id, state, providerId, detail});
	}
}

void ChatConnection::PostMessage(const QString &id, const QString &text, const QNetworkRequest &request,
				 const QJsonObject &body)
{
	QNetworkRequest timed(request);
	timed.setTransferTimeout(15000);
	timed.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	auto *reply = netManager->post(timed, QJsonDocument(body).toJson(QJsonDocument::Compact));
	TrackReply(reply);
	connect(reply, &QNetworkReply::finished, this, [this, reply, id, text] {
		reply->deleteLater();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() != QNetworkReply::NoError) {
			const bool refused = status >= 400 && status < 500;
			const QString reason = status == 401 || status == 403 ? QStringLiteral("auth")
					       : status == 429                ? QStringLiteral("rate-limit")
									      : QStringLiteral("network");
			FinishSend(id, refused ? ChatSendState::Rejected : ChatSendState::Unknown, {}, reason);
			return;
		}
		const auto root = QJsonDocument::fromJson(reply->readAll()).object();
		QJsonObject data;
		QString providerId;
		if (channel.platform == StreamPlatform::YouTube) {
			providerId = root[QStringLiteral("id")].toString();
		} else {
			const auto value = root[QStringLiteral("data")];
			const auto array = value.toArray();
			data = value.isArray() ? (array.isEmpty() ? QJsonObject() : array.first().toObject())
					       : value.toObject();
			if (data.contains(QStringLiteral("is_sent")) && !data[QStringLiteral("is_sent")].toBool()) {
				FinishSend(id, ChatSendState::Rejected, {}, QStringLiteral("rejected"));
				return;
			}
			if (data[QStringLiteral("is_sent")].toBool()) {
				providerId = data[QStringLiteral("message_id")].toString();
			}
		}
		if (providerId.isEmpty()) {
			FinishSend(id, ChatSendState::Unknown, {}, QStringLiteral("invalid-response"));
			return;
		}
		ChatMessage echo = NewMessage();
		echo.id = providerId;
		echo.senderName = DisplayName();
		echo.senderId = channel.accountId;
		echo.messageText = text;
		echo.isBroadcaster = true;
		echo.roleBadges = BuildRoleBadges(echo);
		emit messageReceived(echo);
		FinishSend(id, ChatSendState::Delivered, providerId);
	});
}
