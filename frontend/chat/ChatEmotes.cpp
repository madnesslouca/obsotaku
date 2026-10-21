/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatEmotes.hpp"
#include <QBuffer>
#include <QDateTime>
#include <QImageReader>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include "moc_ChatEmotes.cpp"

namespace ChatEmotes {
QUrl ImageUrl(const ChatEmote &emote)
{
	static const QRegularExpression twitch(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
	static const QRegularExpression kick(QStringLiteral("^[0-9]{1,20}$"));
	if (emote.platform == StreamPlatform::Twitch && twitch.match(emote.id).hasMatch()) {
		return QUrl(
			QStringLiteral("https://static-cdn.jtvnw.net/emoticons/v2/%1/static/dark/2.0").arg(emote.id));
	}
	if (emote.platform == StreamPlatform::Kick && kick.match(emote.id).hasMatch()) {
		return QUrl(QStringLiteral("https://files.kick.com/emotes/%1/fullsize").arg(emote.id));
	}
	return {};
}
QString ResourceName(const ChatEmote &emote)
{
	if (ImageUrl(emote).isEmpty()) {
		return {};
	}
	return QStringLiteral("chatemote:%1:%2").arg(int(emote.platform)).arg(emote.id);
}
void ParseTwitch(ChatMessage &message, const QString &tag)
{
	// IRC positions count Unicode characters. Normalize to QString UTF-16 offsets.
	QList<int> offsets;
	for (int i = 0; i < message.messageText.size(); ++i) {
		offsets.append(i);
		if (message.messageText[i].isHighSurrogate() && i + 1 < message.messageText.size() &&
		    message.messageText[i + 1].isLowSurrogate()) {
			++i;
		}
	}
	offsets.append(message.messageText.size());
	for (const auto &part : tag.split('/')) {
		const int colon = part.indexOf(':');
		if (colon < 1) {
			continue;
		}
		const auto id = part.left(colon);
		if (ImageUrl({0, 1, StreamPlatform::Twitch, id}).isEmpty()) {
			continue;
		}
		for (const auto &range : part.mid(colon + 1).split(',')) {
			bool okStart = false, okEnd = false;
			const auto ends = range.split('-');
			if (ends.size() != 2) {
				continue;
			}
			int start = ends[0].toInt(&okStart), end = ends[1].toInt(&okEnd);
			if (!okStart || !okEnd || start < 0 || end < start || end >= offsets.size() - 1 ||
			    message.emotes.size() >= 128) {
				continue;
			}
			message.emotes.append(
				{offsets[start], offsets[end + 1] - offsets[start], StreamPlatform::Twitch, id});
		}
	}
}
void ParseKick(ChatMessage &message)
{
	static const QRegularExpression token(QStringLiteral("\\[emote:([0-9]{1,20}):([^\\]\\r\\n]{1,100})\\]"));
	auto matches = token.globalMatch(message.messageText);
	while (matches.hasNext() && message.emotes.size() < 128) {
		const auto m = matches.next();
		message.emotes.append(
			{int(m.capturedStart()), int(m.capturedLength()), StreamPlatform::Kick, m.captured(1)});
	}
}
void ParseTwitchFragments(ChatMessage &message, const QJsonArray &fragments)
{
	message.messageText.clear();
	message.emotes.clear();
	for (const auto &value : fragments) {
		const auto fragment = value.toObject();
		const auto text = fragment["text"].toString();
		if (message.messageText.size() + text.size() > 16000) {
			break;
		}
		if (fragment["type"].toString() == QStringLiteral("emote") && message.emotes.size() < 128) {
			message.emotes.append({int(message.messageText.size()), int(text.size()),
					       StreamPlatform::Twitch, fragment["emote"].toObject()["id"].toString()});
		}
		message.messageText += text;
	}
}
} // namespace ChatEmotes

ChatEmoteCache::ChatEmoteCache(QObject *parent, QNetworkAccessManager *manager)
	: QObject(parent),
	  network(manager ? manager : new QNetworkAccessManager(this))
{
}
ChatEmoteCache::~ChatEmoteCache()
{
	Stop();
}
void ChatEmoteCache::Stop()
{
	stopped = true;
	queue.clear();
	queued.clear();
	const auto replies = active;
	active.clear();
	for (auto *reply : replies) {
		reply->disconnect(this);
		reply->abort();
		reply->deleteLater();
	}
}
QString ChatEmoteCache::Html(const ChatMessage &message, QTextDocument *document, int height)
{
	auto emotes = message.emotes;
	std::sort(emotes.begin(), emotes.end(),
		  [](const ChatEmote &a, const ChatEmote &b) { return a.start < b.start; });
	int offset = 0;
	QString html;
	for (const auto &emote : emotes) {
		if (emote.start < offset || emote.length <= 0 || emote.start > message.messageText.size() ||
		    emote.length > message.messageText.size() - emote.start) {
			continue;
		}
		html += message.messageText.mid(offset, emote.start - offset).toHtmlEscaped();
		const auto name = ChatEmotes::ResourceName(emote);
		const auto text = message.messageText.mid(emote.start, emote.length);
		if (!name.isEmpty() && images.contains(name)) {
			const auto &image = *images.object(name);
			document->addResource(QTextDocument::ImageResource, QUrl(name), image);
			const int width = std::clamp(qRound(height * double(image.width()) / image.height()), 1, 96);
			html += QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\" alt=\"%4\" title=\"%4\">")
					.arg(name)
					.arg(width)
					.arg(height)
					.arg(text.toHtmlEscaped());
		} else {
			html += text.toHtmlEscaped();
			if (!name.isEmpty()) {
				Request(emote);
			}
		}
		offset = emote.start + emote.length;
	}
	return html + message.messageText.mid(offset).toHtmlEscaped();
}
void ChatEmoteCache::Request(const ChatEmote &emote)
{
	if (stopped) {
		return;
	}
	const auto name = ChatEmotes::ResourceName(emote);
	if (name.isEmpty() || queued.contains(name) || images.contains(name) || queue.size() >= 256) {
		return;
	}
	const auto now = QDateTime::currentSecsSinceEpoch();
	if (failures.value(name) > now) {
		return;
	}
	if (now / 60 != requestMinute) {
		requestMinute = now / 60;
		requestsThisMinute = 0;
	}
	if (requestsThisMinute >= 256) {
		return;
	}
	if (failures.size() >= 4096) {
		for (auto it = failures.begin(); it != failures.end();) {
			if (it.value() <= now) {
				it = failures.erase(it);
			} else {
				++it;
			}
		}
		if (failures.size() >= 4096) {
			return;
		}
	}
	++requestsThisMinute;
	// Successful but evicted images also cool down, preventing cache thrash.
	failures[name] = now + 300;
	queued.insert(name);
	queue.enqueue(emote);
	Pump();
}
void ChatEmoteCache::Pump()
{
	while (!stopped && active.size() < 4 && !queue.isEmpty()) {
		const auto emote = queue.dequeue();
		const auto name = ChatEmotes::ResourceName(emote);
		QNetworkRequest request(ChatEmotes::ImageUrl(emote));
		request.setTransferTimeout(10000);
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
		auto *reply = network->get(request);
		active.insert(reply);
		// Abort as data arrives rather than buffering an unbounded response.
		connect(reply, &QNetworkReply::readyRead, this, [reply] {
			if (reply->bytesAvailable() > 512 * 1024) {
				reply->abort();
			}
		});
		connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
			if (received > 512 * 1024 || total > 512 * 1024) {
				reply->abort();
			}
		});
		connect(reply, &QNetworkReply::finished, this, [this, reply, name] {
			active.remove(reply);
			queued.remove(name);
			reply->deleteLater();
			const auto bytes = reply->readAll();
			bool loaded = false;
			if (reply->error() == QNetworkReply::NoError &&
			    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
			    bytes.size() <= 512 * 1024) {
				const bool raster = bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) ||
						    bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a") ||
						    bytes.startsWith(QByteArray::fromHex("ffd8ff")) ||
						    (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP");
				if (!raster) {
					failures[name] = QDateTime::currentSecsSinceEpoch() + 300;
					Pump();
					return;
				}
				QBuffer buffer;
				buffer.setData(bytes);
				buffer.open(QIODevice::ReadOnly);
				QImageReader reader(&buffer);
				const auto size = reader.size();
				if (size.isValid() && size.width() <= 512 && size.height() <= 512) {
					auto image = reader.read();
					if (!image.isNull()) {
						const int cost = image.sizeInBytes();
						images.insert(name, new QImage(std::move(image)), cost);
						loaded = true;
					}
				}
			}
			if (!loaded) {
				failures[name] = QDateTime::currentSecsSinceEpoch() + 300;
			} else {
				emit imageReady();
			}
			Pump();
		});
	}
}
