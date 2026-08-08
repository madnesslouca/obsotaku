/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "KickChatConnection.hpp"

#include <qt-wrappers.hpp>

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

#include <limits>
#include <vector>

#include "moc_KickChatConnection.cpp"

namespace {
constexpr const char *WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr const char *PUSHER_HOST = "ws-us2.pusher.com";
/* Kick's chat runs on a Pusher app the site itself connects to. The key is not
 * a secret — it is in the page's JavaScript — but it does change: when chat
 * stops arriving, read it back from a kick.com WebSocket request. */
constexpr const char *PUSHER_APP = "32cbd69e4b950bf97679";

QByteArray RandomBytes(int count)
{
	std::vector<quint32> words(static_cast<size_t>((count + 3) / 4));
	QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
	return QByteArray(reinterpret_cast<const char *>(words.data()), count);
}
} // namespace

void KickChatConnection::Start()
{
	if (channel.address.trimmed().isEmpty())
		return;
	EmitStatus(ChatConnectionState::Connecting);
	if (chatroomId.isEmpty())
		ResolveChatroom();
	else
		OpenSocket();
}

void KickChatConnection::Stop()
{
	StopReconnect();
	if (socket) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
		socket = nullptr;
	}
	chatroomId.clear();
	buffer.clear();
	fragment.clear();
	handshakeComplete = false;
	connected = false;
	EmitStatus(ChatConnectionState::Disconnected);
}

void KickChatConnection::ResolveChatroom()
{
	const QString slug = channel.address.trimmed().toLower();
	const QUrl url(QStringLiteral("https://kick.com/api/v2/channels/%1/chatroom").arg(slug));
	QNetworkRequest request(url);
	/* This endpoint sits behind Cloudflare, which turns away clients that do
	 * not look like a browser. Anything less than a full set of browser
	 * headers comes back as 403 and chat never connects. */
	request.setHeader(QNetworkRequest::UserAgentHeader,
			  QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
					 "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"));
	request.setRawHeader("Accept", "application/json, text/plain, */*");
	request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
	request.setRawHeader("Referer", QStringLiteral("https://kick.com/%1").arg(slug).toUtf8());
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

	blog(LOG_INFO, "[multistream chat] resolving Kick chatroom for '%s'", QT_TO_UTF8(slug));

	QNetworkReply *reply = netManager->get(request);
	QPointer<KickChatConnection> guard(this);
	connect(reply, &QNetworkReply::finished, this, [this, guard, reply, slug]() {
		reply->deleteLater();
		if (!guard || channel.address.trimmed().isEmpty())
			return;

		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_WARNING, "[multistream chat] Kick chatroom lookup for '%s' failed: HTTP %d, %s",
			     QT_TO_UTF8(slug), status, QT_TO_UTF8(reply->errorString()));
			EmitStatus(ChatConnectionState::Failed, status == 404 ? slug : reply->errorString());
			ScheduleReconnect();
			return;
		}

		const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		if (doc.isObject()) {
			const QJsonObject obj = doc.object();
			const QJsonValue id = obj.contains(QStringLiteral("id"))
						      ? obj[QStringLiteral("id")]
						      : obj[QStringLiteral("chatroom")].toObject()[QStringLiteral("id")];
			if (!id.isUndefined() && !id.isNull())
				chatroomId = QString::number(id.toVariant().toLongLong());
		}

		if (chatroomId.isEmpty()) {
			blog(LOG_WARNING, "[multistream chat] Kick returned no chatroom id for '%s'", QT_TO_UTF8(slug));
			EmitStatus(ChatConnectionState::Failed, slug);
			ScheduleReconnect();
			return;
		}
		blog(LOG_INFO, "[multistream chat] Kick chatroom %s resolved for '%s'", QT_TO_UTF8(chatroomId),
		     QT_TO_UTF8(slug));
		OpenSocket();
	});
}

void KickChatConnection::OpenSocket()
{
	if (socket) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
	}

	buffer.clear();
	fragment.clear();
	handshakeComplete = false;
	handshakeKey = RandomBytes(16).toBase64();

	socket = new QSslSocket(this);
	connect(socket, &QSslSocket::connected, this, &KickChatConnection::OnConnected);
	connect(socket, &QSslSocket::readyRead, this, &KickChatConnection::OnReadyRead);
	connect(socket, &QSslSocket::errorOccurred, this, &KickChatConnection::OnError);
	connect(socket, &QSslSocket::disconnected, this, &KickChatConnection::OnDisconnected);
	blog(LOG_INFO, "[multistream chat] opening the Kick chat socket to %s", PUSHER_HOST);
	socket->connectToHostEncrypted(QString::fromLatin1(PUSHER_HOST), 443);
}

void KickChatConnection::OnConnected()
{
	if (!socket)
		return;

	const QString handshake =
		QStringLiteral("GET /app/%1?protocol=7&client=obs-multistream&version=1.0 HTTP/1.1\r\n"
			       "Host: %2\r\n"
			       "Upgrade: websocket\r\n"
			       "Connection: Upgrade\r\n"
			       "Sec-WebSocket-Key: %3\r\n"
			       "Sec-WebSocket-Version: 13\r\n\r\n")
			.arg(QString::fromLatin1(PUSHER_APP), QString::fromLatin1(PUSHER_HOST),
			     QString::fromLatin1(handshakeKey));

	socket->write(handshake.toUtf8());
	socket->flush();
}

void KickChatConnection::OnReadyRead()
{
	if (!socket)
		return;

	buffer.append(socket->readAll());
	if (!handshakeComplete && !ProcessHandshake())
		return;
	ProcessFrames();
}

bool KickChatConnection::ProcessHandshake()
{
	const int headerEnd = buffer.indexOf("\r\n\r\n");
	if (headerEnd < 0)
		return false;

	const QByteArray header = buffer.left(headerEnd);
	buffer.remove(0, headerEnd + 4);

	const QByteArray expectedAccept =
		QCryptographicHash::hash(handshakeKey + WEBSOCKET_GUID, QCryptographicHash::Sha1).toBase64();

	if (!header.startsWith("HTTP/1.1 101") || !header.contains(expectedAccept)) {
		blog(LOG_WARNING, "[multistream chat] Kick WebSocket upgrade refused: %s",
		     header.left(header.indexOf("\r\n")).constData());
		EmitStatus(ChatConnectionState::Failed, QString::fromLatin1(header.left(header.indexOf("\r\n"))));
		socket->abort();
		return false;
	}

	handshakeComplete = true;
	connected = true;
	ResetReconnectDelay();
	blog(LOG_INFO, "[multistream chat] Kick chat socket open, subscribing to chatroom %s", QT_TO_UTF8(chatroomId));
	EmitStatus(ChatConnectionState::Connected, DisplayName());

	QJsonObject subData;
	subData[QStringLiteral("auth")] = QString();
	subData[QStringLiteral("channel")] = QStringLiteral("chatrooms.%1.v2").arg(chatroomId);
	QJsonObject subEvent;
	subEvent[QStringLiteral("event")] = QStringLiteral("pusher:subscribe");
	subEvent[QStringLiteral("data")] = subData;
	SendFrame(0x1, QJsonDocument(subEvent).toJson(QJsonDocument::Compact));
	return true;
}

void KickChatConnection::SendFrame(quint8 opcode, const QByteArray &payload)
{
	if (!socket || socket->state() != QAbstractSocket::ConnectedState)
		return;

	QByteArray frame;
	frame.append(static_cast<char>(0x80 | opcode));

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

	socket->write(frame);
	socket->flush();
}

void KickChatConnection::ProcessFrames()
{
	while (buffer.size() >= 2) {
		const quint8 first = static_cast<quint8>(buffer[0]);
		const quint8 second = static_cast<quint8>(buffer[1]);
		const bool fin = (first & 0x80) != 0;
		const quint8 opcode = first & 0x0F;
		const bool masked = (second & 0x80) != 0;
		quint64 length = second & 0x7F;
		qsizetype offset = 2;

		if (length == 126) {
			if (buffer.size() < offset + 2)
				return;
			length = (static_cast<quint8>(buffer[offset]) << 8) | static_cast<quint8>(buffer[offset + 1]);
			offset += 2;
		} else if (length == 127) {
			if (buffer.size() < offset + 8)
				return;
			length = 0;
			for (int index = 0; index < 8; ++index)
				length = (length << 8) | static_cast<quint8>(buffer[offset + index]);
			offset += 8;
		}

		QByteArray mask;
		if (masked) {
			if (buffer.size() < offset + 4)
				return;
			mask = buffer.mid(offset, 4);
			offset += 4;
		}

		if (length > static_cast<quint64>(std::numeric_limits<int>::max())) {
			socket->abort();
			return;
		}
		if (static_cast<quint64>(buffer.size()) < static_cast<quint64>(offset) + length)
			return;

		QByteArray payload = buffer.mid(offset, static_cast<qsizetype>(length));
		buffer.remove(0, offset + static_cast<qsizetype>(length));
		if (masked) {
			for (qsizetype index = 0; index < payload.size(); ++index)
				payload[index] = payload[index] ^ mask[index % 4];
		}

		switch (opcode) {
		case 0x0:
			fragment.append(payload);
			if (fin) {
				if (fragmentOpcode == 0x1)
					HandlePayload(fragment);
				fragment.clear();
				fragmentOpcode = 0;
			}
			break;
		case 0x1:
		case 0x2:
			if (fin) {
				if (opcode == 0x1)
					HandlePayload(payload);
			} else {
				fragmentOpcode = opcode;
				fragment = payload;
			}
			break;
		case 0x8:
			socket->close();
			return;
		case 0x9:
			SendFrame(0xA, payload);
			break;
		case 0xA:
			break;
		default:
			break;
		}
	}
}

void KickChatConnection::HandlePayload(const QByteArray &payload)
{
	const QJsonDocument doc = QJsonDocument::fromJson(payload);
	if (!doc.isObject())
		return;

	const QJsonObject obj = doc.object();
	const QString event = obj[QStringLiteral("event")].toString();

	if (event == QStringLiteral("pusher:error")) {
		blog(LOG_WARNING, "[multistream chat] Kick chat socket refused the subscription: %s",
		     payload.constData());
		return;
	}
	if (event == QStringLiteral("pusher:ping")) {
		QJsonObject pong;
		pong[QStringLiteral("event")] = QStringLiteral("pusher:pong");
		pong[QStringLiteral("data")] = QJsonObject();
		SendFrame(0x1, QJsonDocument(pong).toJson(QJsonDocument::Compact));
		return;
	}
	if (event != QStringLiteral("App\\Events\\ChatMessageEvent"))
		return;

	const QJsonDocument eventDoc = QJsonDocument::fromJson(obj[QStringLiteral("data")].toString().toUtf8());
	if (!eventDoc.isObject())
		return;

	const QJsonObject dataObj = eventDoc.object();
	ChatMessage msg = NewMessage();
	msg.messageText = dataObj[QStringLiteral("content")].toString();

	const QJsonObject sender = dataObj[QStringLiteral("sender")].toObject();
	msg.senderName = sender[QStringLiteral("username")].toString();
	msg.userColor = sender[QStringLiteral("identity")].toObject()[QStringLiteral("color")].toString();

	const QJsonArray badges = sender[QStringLiteral("identity")].toObject()[QStringLiteral("badges")].toArray();
	for (const QJsonValue &badge : badges) {
		const QString type = badge.toObject()[QStringLiteral("type")].toString().toLower();
		if (type.contains(QStringLiteral("mod")))
			msg.isModerator = true;
		else if (type.contains(QStringLiteral("sub")))
			msg.isSubscriber = true;
		else if (type.contains(QStringLiteral("vip")))
			msg.isVip = true;
		else if (type.contains(QStringLiteral("broadcaster")) || type.contains(QStringLiteral("host")))
			msg.isBroadcaster = true;
	}
	msg.roleBadges = BuildRoleBadges(msg);

	if (!msg.senderName.isEmpty() && !msg.messageText.isEmpty())
		emit messageReceived(msg);
}

void KickChatConnection::OnError()
{
	connected = false;
	handshakeComplete = false;
	const QString detail = socket ? socket->errorString() : QString();
	blog(LOG_WARNING, "[multistream chat] Kick chat socket error: %s", QT_TO_UTF8(detail));
	EmitStatus(ChatConnectionState::Failed, detail);
	ScheduleReconnect();
}

void KickChatConnection::OnDisconnected()
{
	blog(LOG_INFO, "[multistream chat] Kick chat socket closed");
	connected = false;
	handshakeComplete = false;
	EmitStatus(ChatConnectionState::Disconnected);
	ScheduleReconnect();
}
