/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once
#include "ChatConnection.hpp"
#include <QCache>
#include <QImage>
#include <QQueue>
#include <QTextDocument>

namespace ChatEmotes {
QUrl ImageUrl(const ChatEmote &emote);
QString ResourceName(const ChatEmote &emote);
void ParseTwitch(ChatMessage &message, const QString &tag);
void ParseKick(ChatMessage &message);
void ParseTwitchFragments(ChatMessage &message, const QJsonArray &fragments);
} // namespace ChatEmotes

// Independent, credential-free image requests; bounded queue, bytes and decoded pixels.
class ChatEmoteCache : public QObject {
	Q_OBJECT
public:
	explicit ChatEmoteCache(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
	~ChatEmoteCache() override;
	QString Html(const ChatMessage &message, QTextDocument *document, int height);
	void Stop();
signals:
	void imageReady();

private:
	void Request(const ChatEmote &emote);
	void Pump();
	QNetworkAccessManager *network;
	QCache<QString, QImage> images{8 * 1024 * 1024};
	QHash<QString, qint64> failures;
	QSet<QString> queued;
	QQueue<ChatEmote> queue;
	QSet<QNetworkReply *> active;
	qint64 requestMinute = -1;
	int requestsThisMinute = 0;
	bool stopped = false;
};
