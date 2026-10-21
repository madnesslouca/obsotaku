/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatEventSocket.hpp"
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QStringDecoder>
#include "moc_ChatEventSocket.cpp"
namespace {
constexpr qsizetype MaxPayload = 1024 * 1024;
QByteArray Random(int count)
{
	QByteArray data(count, Qt::Uninitialized);
	for (auto &byte : data) {
		byte = char(QRandomGenerator::system()->generate() & 255);
	}
	return data;
}
} // namespace
ChatEventSocket::ChatEventSocket(QObject *parent) : QObject(parent)
{
	deadline.setSingleShot(true);
	connect(&deadline, &QTimer::timeout, this, &ChatEventSocket::Fail);
	connect(&socket, &QSslSocket::encrypted, this, [this] {
		auto path = url.path(QUrl::FullyEncoded).toUtf8();
		if (path.isEmpty()) {
			path = "/";
		}
		if (url.hasQuery()) {
			path += "?" + url.query(QUrl::FullyEncoded).toUtf8();
		}
		socket.write("GET " + path + " HTTP/1.1\r\nHost: " + url.host().toUtf8() +
			     "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
			     "\r\nSec-WebSocket-Version: 13\r\n\r\n");
	});
	connect(&socket, &QSslSocket::readyRead, this, &ChatEventSocket::Read);
	connect(&socket, &QSslSocket::errorOccurred, this, &ChatEventSocket::Fail);
	connect(&socket, &QSslSocket::disconnected, this, &ChatEventSocket::Fail);
}
bool ChatEventSocket::AllowedUrl(const QUrl &value)
{
	return value.isValid() && value.scheme() == QStringLiteral("wss") &&
	       value.host() == QStringLiteral("eventsub.wss.twitch.tv") && value.port(443) == 443 &&
	       value.userInfo().isEmpty() && !value.hasFragment();
}
void ChatEventSocket::Open(const QUrl &value)
{
	Stop();
	running = true;
	if (!AllowedUrl(value)) {
		Fail();
		return;
	}
	url = value;
	key = Random(16).toBase64();
	deadline.start(15000);
	socket.setReadBufferSize(MaxPayload + 16384);
	socket.connectToHostEncrypted(url.host(), 443);
}
void ChatEventSocket::Stop()
{
	running = false;
	deadline.stop();
	socket.abort();
	buffer.clear();
	fragments.clear();
	upgraded = false;
	fragmented = false;
}
void ChatEventSocket::Fail()
{
	if (!running) {
		return;
	}
	Stop();
	emit closed();
}
void ChatEventSocket::Read()
{
	const auto data = socket.readAll();
	if (buffer.size() + data.size() > MaxPayload + 16384) {
		Fail();
		return;
	}
	buffer += data;
	if (!upgraded && !Handshake()) {
		return;
	}
	Frames();
}
bool ChatEventSocket::Handshake()
{
	const int end = buffer.indexOf("\r\n\r\n");
	if (end < 0) {
		if (buffer.size() > 16384) {
			Fail();
		}
		return false;
	}
	const auto lines = buffer.left(end).split('\n');
	const auto expected =
		QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1)
			.toBase64();
	bool accept = false, upgrade = false, connection = false;
	for (const auto &raw : lines) {
		const auto line = raw.trimmed();
		const int colon = line.indexOf(':');
		if (colon < 0) {
			continue;
		}
		const auto name = line.left(colon).toLower(), value = line.mid(colon + 1).trimmed();
		if (name == "sec-websocket-accept") {
			accept = value == expected;
		}
		if (name == "upgrade") {
			upgrade = value.toLower() == "websocket";
		}
		if (name == "connection") {
			for (const auto &token : value.toLower().split(',')) {
				connection |= token.trimmed() == "upgrade";
			}
		}
	}
	if (lines.isEmpty() || lines.first().trimmed() != "HTTP/1.1 101 Switching Protocols" || !accept || !upgrade ||
	    !connection) {
		// The status reason phrase is optional; compare the status code separately.
		const auto status = lines.isEmpty() ? QList<QByteArray>() : lines.first().trimmed().split(' ');
		if (status.size() < 2 || status[0] != "HTTP/1.1" || status[1] != "101" || !accept || !upgrade ||
		    !connection) {
			Fail();
			return false;
		}
	}
	buffer.remove(0, end + 4);
	upgraded = true;
	deadline.stop();
	return true;
}
void ChatEventSocket::Control(quint8 opcode, const QByteArray &payload)
{
	if (!running || payload.size() > 125) {
		return;
	}
	QByteArray frame;
	frame += char(0x80 | opcode);
	frame += char(0x80 | payload.size());
	const auto mask = Random(4);
	frame += mask;
	for (qsizetype i = 0; i < payload.size(); ++i) {
		frame += char(payload[i] ^ mask[i % 4]);
	}
	socket.write(frame);
}
void ChatEventSocket::Frames()
{
	while (running && buffer.size() >= 2) {
		const auto a = quint8(buffer[0]), b = quint8(buffer[1]);
		const bool fin = a & 0x80;
		const int op = a & 15;
		quint64 length = b & 127;
		qsizetype offset = 2;
		if ((a & 0x70) || (b & 0x80)) {
			Fail();
			return;
		} // no compression or masked server frames
		if (length == 126) {
			if (buffer.size() < 4) {
				return;
			}
			length = (quint8(buffer[2]) << 8) | quint8(buffer[3]);
			offset = 4;
		} else if (length == 127) {
			if (buffer.size() < 10) {
				return;
			}
			length = 0;
			for (int i = 2; i < 10; ++i) {
				length = (length << 8) | quint8(buffer[i]);
			}
			offset = 10;
		}
		if (length > MaxPayload || (op >= 8 && (!fin || length > 125))) {
			Fail();
			return;
		}
		if (quint64(buffer.size()) < quint64(offset) + length) {
			return;
		}
		const auto data = buffer.mid(offset, qsizetype(length));
		buffer.remove(0, offset + qsizetype(length));
		if (op == 8) {
			Control(8, data);
			Fail();
			return;
		}
		if (op == 9) {
			Control(10, data);
			continue;
		}
		if (op == 10) {
			continue;
		}
		if (op == 1 && !fragmented) {
			fragments = data;
			fragmented = !fin;
		} else if (op == 0 && fragmented) {
			fragments += data;
		} else {
			Fail();
			return;
		}
		if (fragments.size() > MaxPayload) {
			Fail();
			return;
		}
		if (fin) {
			auto message = std::move(fragments);
			fragments.clear();
			fragmented = false;
			QStringDecoder utf8(QStringDecoder::Utf8);
			QString decoded = utf8.decode(message);
			Q_UNUSED(decoded);
			if (utf8.hasError()) {
				Fail();
				return;
			}
			emit textReceived(message);
		}
	}
}
