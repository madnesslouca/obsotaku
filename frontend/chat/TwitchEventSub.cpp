/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "TwitchEventSub.hpp"
#include "ChatEmotes.hpp"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <algorithm>
#include "moc_TwitchEventSub.cpp"

void TwitchEventSub::Start()
{
	Stop();
	BeginSession();
	credentialMissing = false;
	if (!watchdog) {
		watchdog = new QTimer(this);
		watchdog->setSingleShot(true);
		connect(watchdog, &QTimer::timeout, this, &TwitchEventSub::Retry);
	}
	EmitStatus(ChatConnectionState::Connecting);
	WithAccessToken([this](const ChatAccessToken &token) {
		access = token;
		if (access.token.isEmpty() || channel.accountId.isEmpty() ||
		    (!access.scopes.contains("moderator:read:followers") &&
		     !access.scopes.contains("channel:read:redemptions"))) {
			credentialMissing = true;
			EmitStatus(ChatConnectionState::MissingCredential);
			return;
		}
		Open(QUrl(QStringLiteral("wss://eventsub.wss.twitch.tv/ws?keepalive_timeout_seconds=30")), false);
	});
}
void TwitchEventSub::Stop()
{
	EndSession();
	if (watchdog) {
		watchdog->stop();
	}
	for (auto *socket : {current.data(), pending.data()}) {
		if (socket) {
			socket->disconnect(this);
			socket->Stop();
			socket->deleteLater();
		}
	}
	current = nullptr;
	pending = nullptr;
	access = {};
	sessionId.clear();
}
void TwitchEventSub::Retry()
{
	if (stopped) {
		return;
	}
	BeginSession();
	sessionId.clear();
	if (watchdog) {
		watchdog->stop();
	}
	for (auto *socket : {current.data(), pending.data()}) {
		if (socket) {
			socket->disconnect(this);
			socket->Stop();
			socket->deleteLater();
		}
	}
	current = nullptr;
	pending = nullptr;
	connected = false;
	EmitStatus(ChatConnectionState::Failed, QStringLiteral("events-network"));
	ScheduleReconnect();
}
void TwitchEventSub::Open(const QUrl &url, bool transfer)
{
	if (pending) {
		return;
	}
	auto *socket = new ChatEventSocket(this);
	if (transfer) {
		pending = socket;
	} else {
		current = socket;
	}
	connect(socket, &ChatEventSocket::textReceived, this,
		[this, socket](const QByteArray &text) { Handle(socket, text); });
	connect(socket, &ChatEventSocket::closed, this, [this, socket] {
		if (socket == current && pending) {
			current = nullptr;
			socket->deleteLater();
			return;
		}
		Retry();
	});
	watchdog->start(30000);
	socket->Open(url);
}
void TwitchEventSub::Handle(ChatEventSocket *source, const QByteArray &message)
{
	if (stopped || (source != current && source != pending)) {
		return;
	}
	const auto root = QJsonDocument::fromJson(message).object();
	const auto metadata = root["metadata"].toObject();
	const auto type = metadata["message_type"].toString();
	const auto payload = root["payload"].toObject();
	if (type.isEmpty()) {
		Retry();
		return;
	}
	if (type == "session_welcome") {
		if (source != pending && !sessionId.isEmpty()) {
			return;
		}
		const bool transfer = source == pending;
		const auto session = payload["session"].toObject();
		const auto id = session["id"].toString();
		if (id.isEmpty()) {
			Retry();
			return;
		}
		if (transfer) {
			if (current) {
				current->disconnect(this);
				current->Stop();
				current->deleteLater();
			}
			current = pending;
			pending = nullptr;
		}
		sessionId = id;
		const auto seconds = session["keepalive_timeout_seconds"];
		if (seconds.isDouble()) {
			keepaliveMs = std::clamp(seconds.toInt(), 10, 600) * 1000 + 5000;
		}
		watchdog->start(keepaliveMs);
		if (!transfer) {
			Subscribe(); // transferred subscriptions must not be re-created
		}
		return;
	}
	watchdog->start(keepaliveMs);
	if (type == "session_keepalive") {
		return;
	}
	if (type == "session_reconnect") {
		const QUrl url(payload["session"].toObject()["reconnect_url"].toString());
		if (!ChatEventSocket::AllowedUrl(url)) {
			Retry();
			return;
		}
		Open(url, true);
		return;
	}
	if (type == "revocation") {
		const auto subscriptionType = payload["subscription"].toObject()["type"].toString();
		if (subscriptionType == "channel.follow" || subscriptionType.startsWith("channel.channel_points_")) {
			credentialMissing = true;
			EmitStatus(ChatConnectionState::MissingCredential);
		}
		return;
	}
	if (type != "notification") {
		return;
	}
	const auto id = metadata["message_id"].toString();
	if (id.isEmpty() || seen.contains(id)) {
		return;
	}
	seen.insert(id);
	seenOrder.enqueue(id);
	while (seenOrder.size() > 2000) {
		seen.remove(seenOrder.dequeue());
	}
	Notify(root);
}
void TwitchEventSub::Subscribe()
{
	accepted = answered = requested = 0;
	denied = false;
	struct Subscription {
		const char *type;
		const char *version;
		const char *scope;
	};
	const Subscription subscriptions[] = {
		{"channel.follow", "2", "moderator:read:followers"},
		{"channel.channel_points_custom_reward_redemption.add", "1", "channel:read:redemptions"},
		{"channel.channel_points_automatic_reward_redemption.add", "2", "channel:read:redemptions"}};
	missingPermissions = false;
	for (const auto &subscription : subscriptions) {
		if (!access.scopes.contains(subscription.scope)) {
			missingPermissions = true;
			continue;
		}
		++requested;
		QJsonObject condition{{"broadcaster_user_id", channel.accountId}};
		if (QString::fromLatin1(subscription.type) == "channel.follow") {
			condition["moderator_user_id"] = channel.accountId;
		}
		QNetworkRequest request(QUrl(QStringLiteral("https://api.twitch.tv/helix/eventsub/subscriptions")));
		request.setTransferTimeout(10000);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(access.token).toUtf8());
		request.setRawHeader("Client-Id", access.clientId.toUtf8());
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		const QJsonObject body{{"type", subscription.type},
				       {"version", subscription.version},
				       {"condition", condition},
				       {"transport", QJsonObject{{"method", "websocket"}, {"session_id", sessionId}}}};
		auto *reply = netManager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
		TrackReply(reply);
		connect(reply, &QNetworkReply::finished, this, [this, reply] {
			reply->deleteLater();
			++answered;
			const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			denied |= code == 401 || code == 403;
			const auto data = QJsonDocument::fromJson(reply->readAll()).object()["data"].toArray();
			if (reply->error() == QNetworkReply::NoError && code == 202 && !data.isEmpty() &&
			    data.first().toObject()["status"].toString() == "enabled") {
				++accepted;
			}
			if (answered == requested) {
				connected = accepted > 0;
				if (connected) {
					ResetReconnectDelay();
					EmitStatus(ChatConnectionState::Connected,
						   accepted == 3 && !missingPermissions
							   ? QStringLiteral("events-connected")
							   : QStringLiteral("events-partial"));
				} else if (denied) {
					Stop();
					credentialMissing = true;
					EmitStatus(ChatConnectionState::MissingCredential);
				} else {
					Retry();
				}
			}
		});
	}
	if (!requested) {
		Stop();
		credentialMissing = true;
		EmitStatus(ChatConnectionState::MissingCredential);
	}
}
void TwitchEventSub::Notify(const QJsonObject &root)
{
	const auto payload = root["payload"].toObject(), event = payload["event"].toObject();
	if (event["broadcaster_user_id"].toString() != channel.accountId) {
		return;
	}
	const auto type = payload["subscription"].toObject()["type"].toString();
	ChatMessage message = NewMessage();
	message.id = QStringLiteral("eventsub:") + root["metadata"].toObject()["message_id"].toString();
	message.senderId = event["user_id"].toString();
	message.senderName = event["user_name"].toString(event["user_login"].toString());
	const auto time =
		QDateTime::fromString(root["metadata"].toObject()["message_timestamp"].toString(), Qt::ISODate);
	if (time.isValid()) {
		message.timestamp = time.toLocalTime().toString("hh:mm");
	}
	if (type == "channel.follow") {
		message.kind = ChatMessageKind::Follow;
		message.eventType = "follow";
	} else if (type == "channel.channel_points_custom_reward_redemption.add") {
		message.kind = ChatMessageKind::ChannelPoints;
		message.eventType = "points";
		const auto reward = event["reward"].toObject();
		message.eventTitle = reward["title"].toString();
		message.eventCount = reward["cost"].toInt();
		message.messageText = event["user_input"].toString();
	} else if (type == "channel.channel_points_automatic_reward_redemption.add") {
		message.kind = ChatMessageKind::ChannelPoints;
		message.eventType = "points-automatic";
		const auto reward = event["reward"].toObject();
		message.eventCount = reward["channel_points"].toInt();
		message.eventTitle = reward["type"].toString();
		ChatEmotes::ParseTwitchFragments(message, event["message"].toObject()["fragments"].toArray());
	} else {
		return;
	}
	emit messageReceived(message);
}
