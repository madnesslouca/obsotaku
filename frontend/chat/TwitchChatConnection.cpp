/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "TwitchChatConnection.hpp"

#include <dialogs/MultistreamAccountsDialog.hpp>
#include <oauth/OAuthTokenSet.hpp>
#include <oauth/PlatformOAuthClient.hpp>
#include <utility/MultistreamTaskPool.hpp>

#include <QPointer>
#include <QRandomGenerator>

#include "moc_TwitchChatConnection.cpp"

void TwitchChatConnection::Start()
{
	if (channel.address.trimmed().isEmpty())
		return;
	EmitStatus(ChatConnectionState::Connecting);
	LoadTokenAndOpen();
}

void TwitchChatConnection::Stop()
{
	StopReconnect();
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

	const std::string accountId = channel.accountId.toStdString();
	QPointer<TwitchChatConnection> guard(this);
	MultistreamTaskPool().start([guard, accountId]() {
		std::string error;
		auto tokens = OAuthTokenSet::Load(StreamPlatform::Twitch, accountId, error);
		if (tokens && tokens->AccessTokenExpired()) {
			const auto registration =
				MultistreamAccountsDialog::RegistrationForPlatform(StreamPlatform::Twitch);
			OAuthTokenSet refreshed;
			if (PlatformOAuthClient::RefreshTokens(StreamPlatform::Twitch, registration, {}, *tokens,
							       refreshed, error)) {
				refreshed.Save(StreamPlatform::Twitch, accountId, error);
				*tokens = std::move(refreshed);
			} else {
				tokens.reset();
			}
		}

		const QString token = tokens ? QString::fromStdString(tokens->accessToken) : QString();
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, token]() {
				if (!guard || guard->channel.address.trimmed().isEmpty())
					return;
				guard->oauthToken = token;
				guard->authenticated = !token.isEmpty();
				guard->OpenSocket();
			},
			Qt::QueuedConnection);
	});
}

void TwitchChatConnection::OpenSocket()
{
	if (channel.address.trimmed().isEmpty())
		return;
	if (socket) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
	}

	socket = new QSslSocket(this);
	connect(socket, &QSslSocket::connected, this, &TwitchChatConnection::OnConnected);
	connect(socket, &QSslSocket::readyRead, this, &TwitchChatConnection::OnReadyRead);
	connect(socket, &QSslSocket::errorOccurred, this, &TwitchChatConnection::OnError);
	connect(socket, &QSslSocket::disconnected, this, &TwitchChatConnection::OnDisconnected);
	socket->connectToHostEncrypted(QStringLiteral("irc.chat.twitch.tv"), 6697);
}

void TwitchChatConnection::OnConnected()
{
	if (!socket)
		return;

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
	if (!socket)
		return;

	while (socket->canReadLine()) {
		const QString line = QString::fromUtf8(socket->readLine()).trimmed();
		if (line.startsWith(QStringLiteral("PING"))) {
			const QString token = line.mid(4).trimmed();
			socket->write(QStringLiteral("PONG %1\r\n")
					      .arg(token.isEmpty() ? QStringLiteral(":tmi.twitch.tv") : token)
					      .toUtf8());
			socket->flush();
		} else if (line.contains(QStringLiteral("PRIVMSG"))) {
			ParseIrcLine(line);
		} else if (line.contains(QStringLiteral("Login authentication failed")) ||
			   line.contains(QStringLiteral("Login unsuccessful"))) {
			/* Token missing chat scopes or revoked: drop to anonymous. */
			authenticated = false;
			oauthToken.clear();
			EmitStatus(ChatConnectionState::Failed, QStringLiteral("auth"));
			ScheduleReconnect();
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
	ChatMessage msg = NewMessage();

	const int privmsgIdx = line.indexOf(QStringLiteral("PRIVMSG"));
	if (privmsgIdx == -1)
		return;

	const QString tagsPart = line.left(privmsgIdx).trimmed();
	const QString textPart = line.mid(privmsgIdx);

	const int textIdx = textPart.indexOf(QStringLiteral(" :"));
	if (textIdx != -1)
		msg.messageText = textPart.mid(textIdx + 2);
	if (msg.messageText.isEmpty())
		return;

	if (tagsPart.startsWith('@')) {
		const QStringList tags = tagsPart.mid(1).split(';');
		for (const QString &tag : tags) {
			const int eqIdx = tag.indexOf('=');
			if (eqIdx == -1)
				continue;
			const QString key = tag.left(eqIdx);
			const QString value = tag.mid(eqIdx + 1);

			if (key == QStringLiteral("display-name") && !value.isEmpty())
				msg.senderName = value;
			else if (key == QStringLiteral("color") && !value.isEmpty())
				msg.userColor = value;
			else if (key == QStringLiteral("mod") && value == QStringLiteral("1"))
				msg.isModerator = true;
			else if (key == QStringLiteral("subscriber") && value == QStringLiteral("1"))
				msg.isSubscriber = true;
			else if (key == QStringLiteral("vip") && value == QStringLiteral("1"))
				msg.isVip = true;
			else if (key == QStringLiteral("badges")) {
				/* One pass over the whole list: chained else-if on the
				 * same key would stop at the first badge that matched
				 * and drop every other role the chatter holds. */
				msg.isBroadcaster =
					msg.isBroadcaster || value.contains(QStringLiteral("broadcaster/"));
				msg.isModerator = msg.isModerator || value.contains(QStringLiteral("moderator/"));
				msg.isVip = msg.isVip || value.contains(QStringLiteral("vip/"));
				/* Founders are subscribers who joined early; Twitch sends
				 * them a badge of their own and subscriber=0. */
				msg.isSubscriber = msg.isSubscriber ||
						   value.contains(QStringLiteral("subscriber/")) ||
						   value.contains(QStringLiteral("founder/"));
			}
		}
	}

	if (msg.senderName.isEmpty()) {
		const int exclIdx = line.indexOf('!');
		const int colonIdx = line.indexOf(':');
		if (colonIdx != -1 && exclIdx != -1 && exclIdx > colonIdx)
			msg.senderName = line.mid(colonIdx + 1, exclIdx - colonIdx - 1);
	}
	if (msg.senderName.isEmpty())
		return;

	msg.roleBadges = BuildRoleBadges(msg);
	emit messageReceived(msg);
}

bool TwitchChatConnection::CanSend() const
{
	return connected && authenticated && socket;
}

bool TwitchChatConnection::SendText(const QString &text, QString &error)
{
	if (!CanSend()) {
		error = "twitch-anonymous";
		return false;
	}

	const QString target = channel.address.trimmed().toLower();
	socket->write(QStringLiteral("PRIVMSG #%1 :%2\r\n").arg(target, text).toUtf8());
	socket->flush();

	ChatMessage echo = NewMessage();
	echo.senderName = ircNick.isEmpty() ? target : ircNick;
	echo.messageText = text;
	echo.isBroadcaster = true;
	echo.roleBadges = BuildRoleBadges(echo);
	emit messageReceived(echo);

	error.clear();
	return true;
}
