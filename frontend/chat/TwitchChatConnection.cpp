/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "TwitchChatConnection.hpp"
#include "ChatEmotes.hpp"

#include <QPointer>
#include <QRandomGenerator>
#include <QDateTime>

#include "moc_TwitchChatConnection.cpp"

void TwitchChatConnection::Start()
{
	if (channel.address.trimmed().isEmpty()) {
		return;
	}
	BeginSession();
	EmitStatus(ChatConnectionState::Connecting);
	LoadTokenAndOpen();
}

void TwitchChatConnection::Stop()
{
	EndSession();
	if (socket) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
		socket = nullptr;
	}
	oauthToken.clear();
	ircNick.clear();
	authenticated = false;
	connected = false;
	EmitStatus(ChatConnectionState::Disconnected);
}

void TwitchChatConnection::LoadTokenAndOpen()
{
	/* Prefer an authenticated IRC session so the streamer can reply. Without
	 * a stored token (or without chat scopes) fall back to justinfan. */
	if (channel.accountId.isEmpty()) {
		oauthToken.clear();
		authenticated = false;
		OpenSocket();
		return;
	}

	WithAccessToken([this](const ChatAccessToken &access) {
		oauthToken = access.token;
		authenticated = !oauthToken.isEmpty();
		OpenSocket();
	});
}

void TwitchChatConnection::OpenSocket()
{
	if (channel.address.trimmed().isEmpty()) {
		return;
	}
	if (socket) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
	}

	socket = new QSslSocket(this);
	connect(socket, &QSslSocket::encrypted, this, &TwitchChatConnection::OnConnected);
	connect(socket, &QSslSocket::readyRead, this, &TwitchChatConnection::OnReadyRead);
	connect(socket, &QSslSocket::errorOccurred, this, &TwitchChatConnection::OnError);
	connect(socket, &QSslSocket::disconnected, this, &TwitchChatConnection::OnDisconnected);
	socket->connectToHostEncrypted(QStringLiteral("irc.chat.twitch.tv"), 6697);
}

void TwitchChatConnection::OnConnected()
{
	if (!socket) {
		return;
	}

	socket->write("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");

	if (authenticated && !oauthToken.isEmpty()) {
		/* The nick has to be the account's Twitch login, which is what the
		 * chat address holds. Taking it from the display name instead meant
		 * that renaming a channel produced a nick Twitch rejects, and the
		 * session dropped to anonymous: reading kept working and sending
		 * quietly stopped. */
		ircNick = channel.address.trimmed().toLower();
		socket->write(QStringLiteral("PASS oauth:%1\r\n").arg(oauthToken).toUtf8());
		socket->write(QStringLiteral("NICK %1\r\n").arg(ircNick).toUtf8());
	} else {
		const quint32 randomId = QRandomGenerator::global()->bounded(10000, 99999);
		ircNick = QStringLiteral("justinfan%1").arg(randomId);
		authenticated = false;
		/* Anonymous read-only login. Twitch expects SCHMOOPIIE for justinfan. */
		socket->write("PASS SCHMOOPIIE\r\n");
		socket->write(QStringLiteral("NICK %1\r\n").arg(ircNick).toUtf8());
	}

	socket->write(QStringLiteral("JOIN #%1\r\n").arg(channel.address.trimmed().toLower()).toUtf8());
	socket->flush();

	connected = true;
	ResetReconnectDelay();
	EmitStatus(ChatConnectionState::Connected, DisplayName());
}

void TwitchChatConnection::OnReadyRead()
{
	if (!socket) {
		return;
	}

	while (socket->canReadLine()) {
		const QString line = QString::fromUtf8(socket->readLine()).trimmed();
		if (line.startsWith(QStringLiteral("PING"))) {
			const QString token = line.mid(4).trimmed();
			socket->write(QStringLiteral("PONG %1\r\n")
					      .arg(token.isEmpty() ? QStringLiteral(":tmi.twitch.tv") : token)
					      .toUtf8());
			socket->flush();
		} else {
			QString command = line;
			if (command.startsWith('@')) {
				command = command.section(' ', 1);
			}
			if (command.startsWith(':')) {
				command = command.section(' ', 1);
			}
			if (command.section(' ', 0, 0) == QStringLiteral("NOTICE") &&
			    (line.contains(QStringLiteral("Login authentication failed")) ||
			     line.contains(QStringLiteral("Login unsuccessful")))) {
				authenticated = false;
				oauthToken.clear();
				EmitStatus(ChatConnectionState::Failed, QStringLiteral("auth"));
				ScheduleReconnect();
			} else {
				ParseIrcLine(line);
			}
		}
	}
}

void TwitchChatConnection::OnError()
{
	connected = false;
	EmitStatus(ChatConnectionState::Failed, socket ? socket->errorString() : QString());
	ScheduleReconnect();
}

void TwitchChatConnection::OnDisconnected()
{
	connected = false;
	EmitStatus(ChatConnectionState::Disconnected);
	ScheduleReconnect();
}

void TwitchChatConnection::ParseIrcLine(const QString &line)
{
	QString remainder = line;
	QHash<QString, QString> tags;
	if (remainder.startsWith('@')) {
		const auto raw = remainder.section(' ', 0, 0).mid(1).split(';');
		for (const auto &tag : raw) {
			const int eq = tag.indexOf('=');
			if (eq < 0) {
				continue;
			}
			QString value, encoded = tag.mid(eq + 1);
			for (qsizetype i = 0; i < encoded.size(); ++i) {
				if (encoded[i] == QLatin1Char('\\') && i + 1 < encoded.size()) {
					const auto next = encoded[++i];
					value += next == QLatin1Char('s')   ? QLatin1Char(' ')
						 : next == QLatin1Char(':') ? QLatin1Char(';')
						 : next == QLatin1Char('r') ? QLatin1Char(' ')
						 : next == QLatin1Char('n') ? QLatin1Char(' ')
									    : next;
				} else {
					value += encoded[i];
				}
			}
			tags[tag.left(eq)] = value;
		}
		remainder = remainder.section(' ', 1);
	}
	if (remainder.startsWith(':')) {
		remainder = remainder.section(' ', 1);
	}
	const QString command = remainder.section(' ', 0, 0);
	if (command == QStringLiteral("CLEARMSG")) {
		const auto id = tags.value(QStringLiteral("target-msg-id"));
		if (!id.isEmpty()) {
			emit messageRemoved(channel.channelId, id, {});
		}
		return;
	}
	if (command == QStringLiteral("CLEARCHAT")) {
		// A missing target means clear the channel, never another destination.
		emit messageRemoved(channel.channelId, {}, tags.value(QStringLiteral("target-user-id")));
		return;
	}
	if (command == QStringLiteral("USERNOTICE")) {
		ChatMessage event = NewMessage();
		event.id = tags.value(QStringLiteral("id"));
		event.senderId = tags.value(QStringLiteral("user-id"));
		event.senderName = tags.value(QStringLiteral("display-name"), tags.value(QStringLiteral("login")));
		const auto type = tags.value(QStringLiteral("msg-id"));
		const int textStart = remainder.indexOf(QStringLiteral(" :"));
		if (textStart >= 0) {
			event.messageText = remainder.mid(textStart + 2);
		}
		if (type == "raid") {
			event.kind = ChatMessageKind::Raid;
			event.eventType = "raid";
			event.eventCount = tags.value("msg-param-viewerCount").toInt();
		} else if (type.contains("sub") || type.contains("giftpaidupgrade")) {
			event.kind = ChatMessageKind::Membership;
			event.eventType = (type == "giftpaidupgrade" || type == "anongiftpaidupgrade")
						  ? QStringLiteral("subscription")
					  : type.contains("gift") ? "subscription-gift"
					  : type == "resub"       ? "resubscription"
								  : "subscription";
			event.eventCount = type.contains("mysterygift")
						   ? tags.value("msg-param-mass-gift-count").toInt()
						   : tags.value("msg-param-cumulative-months").toInt();
			event.eventTitle = tags.value("msg-param-recipient-display-name");
		} else {
			event.kind = ChatMessageKind::System;
			event.messageText =
				tags.value("system-msg") +
				(event.messageText.isEmpty() ? QString() : QStringLiteral(" · ") + event.messageText);
		}
		ChatEmotes::ParseTwitch(event, tags.value("emotes"));
		if (!event.messageText.isEmpty() || !event.eventType.isEmpty()) {
			emit messageReceived(event);
		}
		return;
	}
	if (command != QStringLiteral("PRIVMSG")) {
		return;
	}
	ChatMessage msg = NewMessage();

	const int privmsgIdx = line.size() - remainder.size();
	if (privmsgIdx == -1) {
		return;
	}

	const QString tagsPart = line.left(privmsgIdx).trimmed();
	const QString textPart = line.mid(privmsgIdx);

	const int textIdx = textPart.indexOf(QStringLiteral(" :"));
	if (textIdx != -1) {
		msg.messageText = textPart.mid(textIdx + 2);
	}
	if (msg.messageText.isEmpty()) {
		return;
	}

	if (tagsPart.startsWith('@')) {
		const QStringList tags = tagsPart.mid(1).section(' ', 0, 0).split(';');
		for (const QString &tag : tags) {
			const int eqIdx = tag.indexOf('=');
			if (eqIdx == -1) {
				continue;
			}
			const QString key = tag.left(eqIdx);
			QString value = tag.mid(eqIdx + 1);
			value.replace(QStringLiteral("\\s"), QStringLiteral(" "));
			value.replace(QStringLiteral("\\:"), QStringLiteral(";"));
			if (key == QStringLiteral("id")) {
				msg.id = value;
			}
			if (key == QStringLiteral("user-id")) {
				msg.senderId = value;
			}

			if (key == QStringLiteral("display-name") && !value.isEmpty()) {
				msg.senderName = value;
			} else if (key == QStringLiteral("color") && !value.isEmpty()) {
				msg.userColor = value;
			} else if (key == QStringLiteral("mod") && value == QStringLiteral("1")) {
				msg.isModerator = true;
			} else if (key == QStringLiteral("subscriber") && value == QStringLiteral("1")) {
				msg.isSubscriber = true;
			} else if (key == QStringLiteral("vip") && value == QStringLiteral("1")) {
				msg.isVip = true;
			} else if (key == QStringLiteral("badges")) {
				/* One pass over the whole list: chained else-if on the
				 * same key would stop at the first badge that matched
				 * and drop every other role the chatter holds. */
				msg.isBroadcaster = msg.isBroadcaster || value.contains(QStringLiteral("broadcaster/"));
				msg.isModerator = msg.isModerator || value.contains(QStringLiteral("moderator/"));
				msg.isVip = msg.isVip || value.contains(QStringLiteral("vip/"));
				/* Founders are subscribers who joined early; Twitch sends
				 * them a badge of their own and subscriber=0. */
				msg.isSubscriber = msg.isSubscriber || value.contains(QStringLiteral("subscriber/")) ||
						   value.contains(QStringLiteral("founder/"));
			}
		}
	}

	if (msg.senderName.isEmpty()) {
		const int exclIdx = line.indexOf('!');
		const int colonIdx = line.indexOf(':');
		if (colonIdx != -1 && exclIdx != -1 && exclIdx > colonIdx) {
			msg.senderName = line.mid(colonIdx + 1, exclIdx - colonIdx - 1);
		}
	}
	if (msg.senderName.isEmpty()) {
		return;
	}

	msg.id = tags.value(QStringLiteral("id"));
	msg.senderId = tags.value(QStringLiteral("user-id"));
	msg.senderName = tags.value(QStringLiteral("display-name"), msg.senderName);
	bool validTime = false;
	const auto millis = tags.value(QStringLiteral("tmi-sent-ts")).toLongLong(&validTime);
	if (validTime) {
		msg.timestamp = QDateTime::fromMSecsSinceEpoch(millis).toString(QStringLiteral("hh:mm"));
	}
	if (tags.value(QStringLiteral("bits")).toInt() > 0) {
		msg.kind = ChatMessageKind::SuperChat;
		msg.paidAmount = tags.value(QStringLiteral("bits"));
		msg.paidCurrency = QStringLiteral("Bits");
	}
	if (msg.messageText.startsWith(QStringLiteral("\001ACTION ")) && msg.messageText.endsWith(QChar(1))) {
		msg.messageText = msg.messageText.mid(8, msg.messageText.size() - 9);
	}
	ChatEmotes::ParseTwitch(msg, tags.value(QStringLiteral("emotes")));
	msg.roleBadges = BuildRoleBadges(msg);
	emit messageReceived(msg);
}

bool TwitchChatConnection::CanSend() const
{
	return connected && authenticated && socket;
}

bool TwitchChatConnection::SendText(const QString &text, const QString &requestId, QString &error)
{
	if (stopped || !CanSend()) {
		error = "twitch-anonymous";
		return false;
	}
	if (text.toUcs4().size() > 500) {
		error = "too-long";
		return false;
	}
	BeginSend(requestId);
	WithAccessToken([this, text, requestId](const ChatAccessToken &access) {
		if (access.token.isEmpty() || channel.accountId.isEmpty()) {
			FinishSend(requestId, ChatSendState::Rejected, {}, QStringLiteral("auth"));
			return;
		}
		// Every destination is the authenticated account's own channel.
		QNetworkRequest request(QUrl(QStringLiteral("https://api.twitch.tv/helix/chat/messages")));
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(access.token).toUtf8());
		request.setRawHeader("Client-Id", access.clientId.toUtf8());
		PostMessage(
			requestId, text, request,
			{{"broadcaster_id", channel.accountId}, {"sender_id", channel.accountId}, {"message", text}});
	});
	error.clear();
	return true;
}
