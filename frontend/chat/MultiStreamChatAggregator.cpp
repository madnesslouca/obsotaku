/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultiStreamChatAggregator.hpp"

#include <dialogs/MultistreamAccountsDialog.hpp>
#include <oauth/OAuthTokenSet.hpp>
#include <oauth/PlatformOAuthClient.hpp>
#include <utility/MultistreamTaskPool.hpp>

#include <util/base.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "moc_MultiStreamChatAggregator.cpp"

namespace {
constexpr const char *WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr const char *KICK_PUSHER_HOST = "ws-us2.pusher.com";
constexpr const char *KICK_PUSHER_APP = "eb1d5f28b26d2f4da0e5";
constexpr int MAX_RECONNECT_DELAY_MS = 60000;
constexpr int YOUTUBE_MIN_POLL_MS = 5000;
constexpr int YOUTUBE_BROADCAST_RETRY_MS = 30000;

QByteArray RandomBytes(int count)
{
	std::vector<quint32> words(static_cast<size_t>((count + 3) / 4));
	QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
	return QByteArray(reinterpret_cast<const char *>(words.data()), count);
}

qint64 CurrentUnixTime()
{
	return QDateTime::currentSecsSinceEpoch();
}

QString CurrentTimestamp()
{
	return QDateTime::currentDateTime().toString(QStringLiteral("hh:mm"));
}
} // namespace

MultiStreamChatAggregator::MultiStreamChatAggregator(QObject *parent) : QObject(parent)
{
	netManager = new QNetworkAccessManager(this);

	ytPollTimer = new QTimer(this);
	ytPollTimer->setSingleShot(true);
	connect(ytPollTimer, &QTimer::timeout, this, &MultiStreamChatAggregator::PollYouTubeChat);

	twitchReconnectTimer = new QTimer(this);
	twitchReconnectTimer->setSingleShot(true);
	connect(twitchReconnectTimer, &QTimer::timeout, this, &MultiStreamChatAggregator::StartTwitch);

	kickReconnectTimer = new QTimer(this);
	kickReconnectTimer->setSingleShot(true);
	connect(kickReconnectTimer, &QTimer::timeout, this, &MultiStreamChatAggregator::StartKick);
}

MultiStreamChatAggregator::~MultiStreamChatAggregator()
{
	DisconnectAll();
}

void MultiStreamChatAggregator::EmitStatus(StreamPlatform platform, ChatConnectionState state, const QString &detail)
{
	emit statusChanged(platform, state, detail);
}

void MultiStreamChatAggregator::ScheduleReconnect(StreamPlatform platform)
{
	/* Exponential backoff, capped: chat is a convenience feature and must
	 * never turn into a reconnect storm against a platform. */
	if (platform == StreamPlatform::Twitch) {
		if (twitchChannel.isEmpty())
			return;
		twitchReconnectTimer->start(twitchReconnectDelayMs);
		twitchReconnectDelayMs = std::min(twitchReconnectDelayMs * 2, MAX_RECONNECT_DELAY_MS);
	} else if (platform == StreamPlatform::Kick) {
		if (kickChannel.isEmpty())
			return;
		kickReconnectTimer->start(kickReconnectDelayMs);
		kickReconnectDelayMs = std::min(kickReconnectDelayMs * 2, MAX_RECONNECT_DELAY_MS);
	}
}

void MultiStreamChatAggregator::ConnectPlatform(StreamPlatform platform, const QString &channelNameOrId)
{
	const QString value = channelNameOrId.trimmed();
	if (value.isEmpty()) {
		EmitStatus(platform, ChatConnectionState::MissingCredential);
		return;
	}

	switch (platform) {
	case StreamPlatform::Twitch:
		DisconnectPlatform(StreamPlatform::Twitch);
		twitchChannel = value.toLower();
		if (twitchChannel.startsWith('#'))
			twitchChannel.remove(0, 1);
		twitchReconnectDelayMs = 5000;
		StartTwitch();
		break;
	case StreamPlatform::Kick:
		DisconnectPlatform(StreamPlatform::Kick);
		kickChannel = value.toLower();
		kickChatroomId.clear();
		kickReconnectDelayMs = 5000;
		StartKick();
		break;
	case StreamPlatform::YouTube:
		DisconnectPlatform(StreamPlatform::YouTube);
		ytAccountId = value;
		ytLiveChatId.clear();
		ytNextPageToken.clear();
		ytPrimed = false;
		StartYouTube();
		break;
	default:
		/* Platforms without a public chat API are reported as unsupported
		 * so the dock can say so instead of silently showing nothing. */
		EmitStatus(platform, ChatConnectionState::Unsupported);
		break;
	}
}

void MultiStreamChatAggregator::DisconnectPlatform(StreamPlatform platform)
{
	switch (platform) {
	case StreamPlatform::Twitch:
		twitchReconnectTimer->stop();
		twitchChannel.clear();
		if (twitchSocket) {
			twitchSocket->disconnect(this);
			twitchSocket->abort();
			twitchSocket->deleteLater();
			twitchSocket = nullptr;
		}
		twitchConnected = false;
		EmitStatus(StreamPlatform::Twitch, ChatConnectionState::Disconnected);
		break;
	case StreamPlatform::Kick:
		kickReconnectTimer->stop();
		kickChannel.clear();
		kickChatroomId.clear();
		kickBuffer.clear();
		kickFragment.clear();
		kickHandshakeComplete = false;
		if (kickSocket) {
			kickSocket->disconnect(this);
			kickSocket->abort();
			kickSocket->deleteLater();
			kickSocket = nullptr;
		}
		kickConnected = false;
		EmitStatus(StreamPlatform::Kick, ChatConnectionState::Disconnected);
		break;
	case StreamPlatform::YouTube:
		ytPollTimer->stop();
		ytAccountId.clear();
		ytLiveChatId.clear();
		ytNextPageToken.clear();
		ytAccessToken.clear();
		ytAccessTokenExpiresAt = 0;
		ytConnected = false;
		EmitStatus(StreamPlatform::YouTube, ChatConnectionState::Disconnected);
		break;
	default:
		break;
	}
}

void MultiStreamChatAggregator::DisconnectAll()
{
	DisconnectPlatform(StreamPlatform::Twitch);
	DisconnectPlatform(StreamPlatform::Kick);
	DisconnectPlatform(StreamPlatform::YouTube);
}

bool MultiStreamChatAggregator::IsConnected(StreamPlatform platform) const
{
	switch (platform) {
	case StreamPlatform::Twitch:
		return twitchConnected;
	case StreamPlatform::Kick:
		return kickConnected;
	case StreamPlatform::YouTube:
		return ytConnected;
	default:
		break;
	}
	return false;
}

// ----------------------------------------------------------------------------
// Twitch IRC
// ----------------------------------------------------------------------------

void MultiStreamChatAggregator::StartTwitch()
{
	if (twitchChannel.isEmpty())
		return;
	if (twitchSocket) {
		twitchSocket->disconnect(this);
		twitchSocket->abort();
		twitchSocket->deleteLater();
	}

	EmitStatus(StreamPlatform::Twitch, ChatConnectionState::Connecting);
	twitchSocket = new QSslSocket(this);
	connect(twitchSocket, &QSslSocket::connected, this, &MultiStreamChatAggregator::OnTwitchConnected);
	connect(twitchSocket, &QSslSocket::readyRead, this, &MultiStreamChatAggregator::OnTwitchReadyRead);
	connect(twitchSocket, &QSslSocket::errorOccurred, this, &MultiStreamChatAggregator::OnTwitchError);
	connect(twitchSocket, &QSslSocket::disconnected, this, &MultiStreamChatAggregator::OnTwitchDisconnected);
	twitchSocket->connectToHostEncrypted(QStringLiteral("irc.chat.twitch.tv"), 6697);
}

void MultiStreamChatAggregator::OnTwitchConnected()
{
	if (!twitchSocket)
		return;

	/* Anonymous read-only login. Twitch expects the literal SCHMOOPIIE
	 * password for justinfan nicknames; a fake oauth token is rejected. */
	const quint32 randomId = QRandomGenerator::global()->bounded(10000, 99999);
	const QString nick = QStringLiteral("justinfan%1").arg(randomId);

	twitchSocket->write("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");
	twitchSocket->write("PASS SCHMOOPIIE\r\n");
	twitchSocket->write(QStringLiteral("NICK %1\r\n").arg(nick).toUtf8());
	twitchSocket->write(QStringLiteral("JOIN #%1\r\n").arg(twitchChannel).toUtf8());
	twitchSocket->flush();

	twitchConnected = true;
	twitchReconnectDelayMs = 5000;
	EmitStatus(StreamPlatform::Twitch, ChatConnectionState::Connected, twitchChannel);
}

void MultiStreamChatAggregator::OnTwitchReadyRead()
{
	if (!twitchSocket)
		return;

	while (twitchSocket->canReadLine()) {
		const QString line = QString::fromUtf8(twitchSocket->readLine()).trimmed();
		if (line.startsWith(QStringLiteral("PING"))) {
			/* Echo the token the server sent instead of a constant. */
			const QString token = line.mid(4).trimmed();
			twitchSocket->write(QStringLiteral("PONG %1\r\n")
						    .arg(token.isEmpty() ? QStringLiteral(":tmi.twitch.tv") : token)
						    .toUtf8());
			twitchSocket->flush();
		} else if (line.contains(QStringLiteral("PRIVMSG"))) {
			ParseTwitchIrcLine(line);
		}
	}
}

void MultiStreamChatAggregator::OnTwitchError(QAbstractSocket::SocketError)
{
	twitchConnected = false;
	const QString detail = twitchSocket ? twitchSocket->errorString() : QString();
	EmitStatus(StreamPlatform::Twitch, ChatConnectionState::Failed, detail);
	ScheduleReconnect(StreamPlatform::Twitch);
}

void MultiStreamChatAggregator::OnTwitchDisconnected()
{
	twitchConnected = false;
	EmitStatus(StreamPlatform::Twitch, ChatConnectionState::Disconnected);
	ScheduleReconnect(StreamPlatform::Twitch);
}

void MultiStreamChatAggregator::ParseTwitchIrcLine(const QString &line)
{
	// @tags :nick!user@host PRIVMSG #channel :text
	ChatMessage msg;
	msg.platform = StreamPlatform::Twitch;
	msg.channelName = twitchChannel;
	msg.timestamp = CurrentTimestamp();

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

	emit messageReceived(msg);
}

// ----------------------------------------------------------------------------
// Kick chat over the Pusher WebSocket
// ----------------------------------------------------------------------------

void MultiStreamChatAggregator::StartKick()
{
	if (kickChannel.isEmpty())
		return;
	EmitStatus(StreamPlatform::Kick, ChatConnectionState::Connecting);
	if (kickChatroomId.isEmpty())
		ResolveKickChatroom();
	else
		OpenKickSocket();
}

void MultiStreamChatAggregator::ResolveKickChatroom()
{
	QNetworkRequest request(
		QUrl(QStringLiteral("https://kick.com/api/v2/channels/%1/chatroom").arg(kickChannel)));
	request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OBS-Multistream/0.1"));
	request.setRawHeader("Accept", "application/json");

	QNetworkReply *reply = netManager->get(request);
	QPointer<MultiStreamChatAggregator> guard(this);
	connect(reply, &QNetworkReply::finished, this, [this, guard, reply]() {
		reply->deleteLater();
		if (!guard || kickChannel.isEmpty())
			return;

		if (reply->error() != QNetworkReply::NoError) {
			EmitStatus(StreamPlatform::Kick, ChatConnectionState::Failed, reply->errorString());
			ScheduleReconnect(StreamPlatform::Kick);
			return;
		}

		const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		if (doc.isObject()) {
			const QJsonObject obj = doc.object();
			const QJsonValue id = obj.contains(QStringLiteral("id")) ? obj[QStringLiteral("id")]
										 : obj[QStringLiteral("chatroom")]
											   .toObject()[QStringLiteral("id")];
			if (!id.isUndefined() && !id.isNull())
				kickChatroomId = QString::number(id.toVariant().toLongLong());
		}

		if (kickChatroomId.isEmpty()) {
			/* Without a numeric chatroom id there is no channel to
			 * subscribe to; guessing one just fails silently. */
			EmitStatus(StreamPlatform::Kick, ChatConnectionState::Failed, kickChannel);
			ScheduleReconnect(StreamPlatform::Kick);
			return;
		}
		OpenKickSocket();
	});
}

void MultiStreamChatAggregator::OpenKickSocket()
{
	if (kickSocket) {
		kickSocket->disconnect(this);
		kickSocket->abort();
		kickSocket->deleteLater();
	}

	kickBuffer.clear();
	kickFragment.clear();
	kickHandshakeComplete = false;
	kickHandshakeKey = RandomBytes(16).toBase64();

	kickSocket = new QSslSocket(this);
	connect(kickSocket, &QSslSocket::connected, this, &MultiStreamChatAggregator::OnKickConnected);
	connect(kickSocket, &QSslSocket::readyRead, this, &MultiStreamChatAggregator::OnKickReadyRead);
	connect(kickSocket, &QSslSocket::errorOccurred, this, &MultiStreamChatAggregator::OnKickError);
	connect(kickSocket, &QSslSocket::disconnected, this, &MultiStreamChatAggregator::OnKickDisconnected);
	kickSocket->connectToHostEncrypted(QString::fromLatin1(KICK_PUSHER_HOST), 443);
}

void MultiStreamChatAggregator::OnKickConnected()
{
	if (!kickSocket)
		return;

	const QString handshake =
		QStringLiteral("GET /app/%1?protocol=7&client=obs-multistream&version=1.0 HTTP/1.1\r\n"
			       "Host: %2\r\n"
			       "Upgrade: websocket\r\n"
			       "Connection: Upgrade\r\n"
			       "Sec-WebSocket-Key: %3\r\n"
			       "Sec-WebSocket-Version: 13\r\n\r\n")
			.arg(QString::fromLatin1(KICK_PUSHER_APP), QString::fromLatin1(KICK_PUSHER_HOST),
			     QString::fromLatin1(kickHandshakeKey));

	kickSocket->write(handshake.toUtf8());
	kickSocket->flush();
}

void MultiStreamChatAggregator::OnKickReadyRead()
{
	if (!kickSocket)
		return;

	kickBuffer.append(kickSocket->readAll());
	if (!kickHandshakeComplete && !ProcessKickHandshake())
		return;
	ProcessKickFrames();
}

bool MultiStreamChatAggregator::ProcessKickHandshake()
{
	const int headerEnd = kickBuffer.indexOf("\r\n\r\n");
	if (headerEnd < 0)
		return false;

	const QByteArray header = kickBuffer.left(headerEnd);
	kickBuffer.remove(0, headerEnd + 4);

	const QByteArray expectedAccept =
		QCryptographicHash::hash(kickHandshakeKey + WEBSOCKET_GUID, QCryptographicHash::Sha1).toBase64();

	if (!header.startsWith("HTTP/1.1 101") || !header.contains(expectedAccept)) {
		/* Either the upgrade was refused or the server did not prove it
		 * saw our key: both mean this is not a usable WebSocket. */
		EmitStatus(StreamPlatform::Kick, ChatConnectionState::Failed,
			   QString::fromLatin1(header.left(header.indexOf("\r\n"))));
		kickSocket->abort();
		return false;
	}

	kickHandshakeComplete = true;
	kickConnected = true;
	kickReconnectDelayMs = 5000;
	EmitStatus(StreamPlatform::Kick, ChatConnectionState::Connected, kickChannel);

	QJsonObject subData;
	subData[QStringLiteral("auth")] = QString();
	subData[QStringLiteral("channel")] = QStringLiteral("chatrooms.%1.v2").arg(kickChatroomId);
	QJsonObject subEvent;
	subEvent[QStringLiteral("event")] = QStringLiteral("pusher:subscribe");
	subEvent[QStringLiteral("data")] = subData;
	SendKickFrame(0x1, QJsonDocument(subEvent).toJson(QJsonDocument::Compact));
	return true;
}

void MultiStreamChatAggregator::SendKickFrame(quint8 opcode, const QByteArray &payload)
{
	if (!kickSocket || kickSocket->state() != QAbstractSocket::ConnectedState)
		return;

	QByteArray frame;
	frame.append(static_cast<char>(0x80 | opcode)); // FIN + opcode

	/* RFC 6455 §5.3: every client frame must be masked, otherwise the server
	 * closes the connection with protocol error 1002. */
	const qsizetype length = payload.size();
	if (length <= 125) {
		frame.append(static_cast<char>(0x80 | length));
	} else if (length <= 0xFFFF) {
		frame.append(static_cast<char>(0x80 | 126));
		frame.append(static_cast<char>((length >> 8) & 0xFF));
		frame.append(static_cast<char>(length & 0xFF));
	} else {
		frame.append(static_cast<char>(0x80 | 127));
		for (int shift = 56; shift >= 0; shift -= 8)
			frame.append(static_cast<char>((length >> shift) & 0xFF));
	}

	const QByteArray mask = RandomBytes(4);
	frame.append(mask);
	QByteArray masked = payload;
	for (qsizetype index = 0; index < masked.size(); ++index)
		masked[index] = masked[index] ^ mask[index % 4];
	frame.append(masked);

	kickSocket->write(frame);
	kickSocket->flush();
}

void MultiStreamChatAggregator::ProcessKickFrames()
{
	while (kickBuffer.size() >= 2) {
		const quint8 first = static_cast<quint8>(kickBuffer[0]);
		const quint8 second = static_cast<quint8>(kickBuffer[1]);
		const bool fin = (first & 0x80) != 0;
		const quint8 opcode = first & 0x0F;
		const bool masked = (second & 0x80) != 0;
		quint64 length = second & 0x7F;
		qsizetype offset = 2;

		if (length == 126) {
			if (kickBuffer.size() < offset + 2)
				return;
			length = (static_cast<quint8>(kickBuffer[offset]) << 8) |
				 static_cast<quint8>(kickBuffer[offset + 1]);
			offset += 2;
		} else if (length == 127) {
			if (kickBuffer.size() < offset + 8)
				return;
			length = 0;
			for (int index = 0; index < 8; ++index)
				length = (length << 8) | static_cast<quint8>(kickBuffer[offset + index]);
			offset += 8;
		}

		QByteArray mask;
		if (masked) {
			if (kickBuffer.size() < offset + 4)
				return;
			mask = kickBuffer.mid(offset, 4);
			offset += 4;
		}

		if (length > static_cast<quint64>(std::numeric_limits<int>::max())) {
			kickSocket->abort();
			return;
		}
		if (static_cast<quint64>(kickBuffer.size()) < static_cast<quint64>(offset) + length)
			return; // wait for the rest of the frame

		QByteArray payload = kickBuffer.mid(offset, static_cast<qsizetype>(length));
		kickBuffer.remove(0, offset + static_cast<qsizetype>(length));
		if (masked) {
			for (qsizetype index = 0; index < payload.size(); ++index)
				payload[index] = payload[index] ^ mask[index % 4];
		}

		switch (opcode) {
		case 0x0: // continuation
			kickFragment.append(payload);
			if (fin) {
				if (kickFragmentOpcode == 0x1)
					HandleKickPayload(kickFragment);
				kickFragment.clear();
				kickFragmentOpcode = 0;
			}
			break;
		case 0x1: // text
		case 0x2: // binary
			if (fin) {
				if (opcode == 0x1)
					HandleKickPayload(payload);
			} else {
				kickFragmentOpcode = opcode;
				kickFragment = payload;
			}
			break;
		case 0x8: // close
			kickSocket->close();
			return;
		case 0x9: // ping
			SendKickFrame(0xA, payload);
			break;
		case 0xA: // pong
			break;
		default:
			break;
		}
	}
}

void MultiStreamChatAggregator::HandleKickPayload(const QByteArray &payload)
{
	const QJsonDocument doc = QJsonDocument::fromJson(payload);
	if (!doc.isObject())
		return;

	const QJsonObject obj = doc.object();
	const QString event = obj[QStringLiteral("event")].toString();

	if (event == QStringLiteral("pusher:ping")) {
		QJsonObject pong;
		pong[QStringLiteral("event")] = QStringLiteral("pusher:pong");
		pong[QStringLiteral("data")] = QJsonObject();
		SendKickFrame(0x1, QJsonDocument(pong).toJson(QJsonDocument::Compact));
		return;
	}
	if (event != QStringLiteral("App\\Events\\ChatMessageEvent"))
		return;

	const QJsonDocument eventDoc = QJsonDocument::fromJson(obj[QStringLiteral("data")].toString().toUtf8());
	if (!eventDoc.isObject())
		return;

	const QJsonObject dataObj = eventDoc.object();
	ChatMessage msg;
	msg.platform = StreamPlatform::Kick;
	msg.channelName = kickChannel;
	msg.timestamp = CurrentTimestamp();
	msg.messageText = dataObj[QStringLiteral("content")].toString();

	const QJsonObject sender = dataObj[QStringLiteral("sender")].toObject();
	msg.senderName = sender[QStringLiteral("username")].toString();
	msg.userColor = sender[QStringLiteral("identity")].toObject()[QStringLiteral("color")].toString();

	if (!msg.senderName.isEmpty() && !msg.messageText.isEmpty())
		emit messageReceived(msg);
}

void MultiStreamChatAggregator::OnKickError(QAbstractSocket::SocketError)
{
	kickConnected = false;
	kickHandshakeComplete = false;
	const QString detail = kickSocket ? kickSocket->errorString() : QString();
	EmitStatus(StreamPlatform::Kick, ChatConnectionState::Failed, detail);
	ScheduleReconnect(StreamPlatform::Kick);
}

void MultiStreamChatAggregator::OnKickDisconnected()
{
	kickConnected = false;
	kickHandshakeComplete = false;
	EmitStatus(StreamPlatform::Kick, ChatConnectionState::Disconnected);
	ScheduleReconnect(StreamPlatform::Kick);
}

// ----------------------------------------------------------------------------
// YouTube live chat
// ----------------------------------------------------------------------------

void MultiStreamChatAggregator::StartYouTube()
{
	if (ytAccountId.isEmpty())
		return;
	EmitStatus(StreamPlatform::YouTube, ChatConnectionState::Connecting);
	ResolveYouTubeLiveChat();
}

void MultiStreamChatAggregator::ScheduleYouTubeRetry(int milliseconds)
{
	if (ytAccountId.isEmpty())
		return;
	ytPollTimer->start(milliseconds);
}

void MultiStreamChatAggregator::WithYouTubeAccessToken(std::function<void(const QString &)> continuation)
{
	/* A valid token is required: the live chat endpoints reject unauthenticated
	 * requests, and an API key alone cannot read another account's chat. */
	if (!ytAccessToken.isEmpty() && CurrentUnixTime() + 60 < ytAccessTokenExpiresAt) {
		continuation(ytAccessToken);
		return;
	}

	const std::string accountId = ytAccountId.toStdString();
	QPointer<MultiStreamChatAggregator> guard(this);
	MultistreamTaskPool().start([guard, accountId, continuation = std::move(continuation)]() mutable {
		std::string error;
		auto tokens = OAuthTokenSet::Load(StreamPlatform::YouTube, accountId, error);
		if (tokens && tokens->AccessTokenExpired()) {
			const auto registration =
				MultistreamAccountsDialog::RegistrationForPlatform(StreamPlatform::YouTube);
			OAuthTokenSet refreshed;
			if (PlatformOAuthClient::RefreshTokens(StreamPlatform::YouTube, registration, {}, *tokens,
							       refreshed, error)) {
				refreshed.Save(StreamPlatform::YouTube, accountId, error);
				*tokens = std::move(refreshed);
			} else {
				tokens.reset();
			}
		}

		const QString token = tokens ? QString::fromStdString(tokens->accessToken) : QString();
		const qint64 expiresAt = tokens ? tokens->expiresAt : 0;
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, token, expiresAt, continuation = std::move(continuation)]() mutable {
				if (!guard)
					return;
				guard->ytAccessToken = token;
				guard->ytAccessTokenExpiresAt = expiresAt;
				continuation(token);
			},
			Qt::QueuedConnection);
	});
}

void MultiStreamChatAggregator::ResolveYouTubeLiveChat()
{
	QPointer<MultiStreamChatAggregator> guard(this);
	WithYouTubeAccessToken([this, guard](const QString &token) {
		if (!guard || ytAccountId.isEmpty())
			return;
		if (token.isEmpty()) {
			ytConnected = false;
			EmitStatus(StreamPlatform::YouTube, ChatConnectionState::MissingCredential);
			return;
		}

		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
		query.addQueryItem(QStringLiteral("broadcastStatus"), QStringLiteral("active"));
		query.addQueryItem(QStringLiteral("broadcastType"), QStringLiteral("all"));
		url.setQuery(query);

		QNetworkRequest request(url);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());
		QNetworkReply *reply = netManager->get(request);
		connect(reply, &QNetworkReply::finished, this, [this, guard, reply]() {
			reply->deleteLater();
			if (!guard || ytAccountId.isEmpty())
				return;
			if (reply->error() != QNetworkReply::NoError) {
				ytConnected = false;
				EmitStatus(StreamPlatform::YouTube, ChatConnectionState::Failed,
					   reply->errorString());
				ScheduleYouTubeRetry(YOUTUBE_BROADCAST_RETRY_MS);
				return;
			}

			const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
			const QJsonArray items = doc.object()[QStringLiteral("items")].toArray();
			for (const QJsonValue &value : items) {
				const QString chatId = value.toObject()[QStringLiteral("snippet")]
							       .toObject()[QStringLiteral("activeLiveChatId")]
							       .toString();
				if (!chatId.isEmpty()) {
					ytLiveChatId = chatId;
					break;
				}
			}

			if (ytLiveChatId.isEmpty()) {
				/* No live broadcast yet; keep checking at a slow pace
				 * instead of burning quota on chat polling. */
				ytConnected = false;
				EmitStatus(StreamPlatform::YouTube, ChatConnectionState::WaitingForBroadcast);
				ScheduleYouTubeRetry(YOUTUBE_BROADCAST_RETRY_MS);
				return;
			}

			ytConnected = true;
			ytPrimed = false;
			ytNextPageToken.clear();
			EmitStatus(StreamPlatform::YouTube, ChatConnectionState::Connected);
			PollYouTubeChat();
		});
	});
}

void MultiStreamChatAggregator::PollYouTubeChat()
{
	if (ytAccountId.isEmpty())
		return;
	if (ytLiveChatId.isEmpty()) {
		ResolveYouTubeLiveChat();
		return;
	}
	if (ytRequestInFlight)
		return;

	QPointer<MultiStreamChatAggregator> guard(this);
	WithYouTubeAccessToken([this, guard](const QString &token) {
		if (!guard || ytLiveChatId.isEmpty())
			return;
		if (token.isEmpty()) {
			ytConnected = false;
			EmitStatus(StreamPlatform::YouTube, ChatConnectionState::MissingCredential);
			return;
		}

		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("liveChatId"), ytLiveChatId);
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,authorDetails"));
		if (!ytNextPageToken.isEmpty())
			query.addQueryItem(QStringLiteral("pageToken"), ytNextPageToken);
		url.setQuery(query);

		QNetworkRequest request(url);
		request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());
		ytRequestInFlight = true;
		QNetworkReply *reply = netManager->get(request);
		connect(reply, &QNetworkReply::finished, this, [this, guard, reply]() {
			reply->deleteLater();
			if (!guard)
				return;
			ytRequestInFlight = false;
			if (ytLiveChatId.isEmpty())
				return;

			if (reply->error() != QNetworkReply::NoError) {
				const int status =
					reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
				if (status == 403 || status == 404) {
					/* The broadcast ended or the chat is gone: go
					 * back to looking for an active broadcast. */
					ytLiveChatId.clear();
					ytConnected = false;
					EmitStatus(StreamPlatform::YouTube,
						   ChatConnectionState::WaitingForBroadcast);
				} else {
					EmitStatus(StreamPlatform::YouTube, ChatConnectionState::Failed,
						   reply->errorString());
				}
				ScheduleYouTubeRetry(YOUTUBE_BROADCAST_RETRY_MS);
				return;
			}

			const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
			const QJsonObject obj = doc.object();
			ytNextPageToken = obj[QStringLiteral("nextPageToken")].toString();

			/* The first page is chat backlog. Showing it would dump
			 * hundreds of old messages into the dock at once. */
			const bool emitMessages = ytPrimed;
			ytPrimed = true;

			if (emitMessages) {
				for (const QJsonValue &value : obj[QStringLiteral("items")].toArray()) {
					const QJsonObject item = value.toObject();
					ChatMessage msg;
					msg.platform = StreamPlatform::YouTube;
					msg.channelName = QStringLiteral("YouTube");
					msg.timestamp = CurrentTimestamp();
					msg.messageText = item[QStringLiteral("snippet")]
								  .toObject()[QStringLiteral("displayMessage")]
								  .toString();

					const QJsonObject author = item[QStringLiteral("authorDetails")].toObject();
					msg.senderName = author[QStringLiteral("displayName")].toString();
					msg.isModerator = author[QStringLiteral("isChatModerator")].toBool();
					msg.isSubscriber = author[QStringLiteral("isChatSponsor")].toBool();

					if (!msg.senderName.isEmpty() && !msg.messageText.isEmpty())
						emit messageReceived(msg);
				}
			}

			/* Respect the interval the API asks for; a fixed 3 s poll
			 * exhausts the daily quota in under two hours. */
			const int suggested = obj[QStringLiteral("pollingIntervalMillis")].toInt(YOUTUBE_MIN_POLL_MS);
			ScheduleYouTubeRetry(std::max(suggested, YOUTUBE_MIN_POLL_MS));
		});
	});
}
