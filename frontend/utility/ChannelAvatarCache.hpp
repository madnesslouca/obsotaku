/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QString>

class QNetworkAccessManager;

/* Fetches channel profile pictures once and keeps them on disk, so the channel
 * bar does not hit the platform on every start. Emits avatarReady when a
 * download finishes; callers draw the fallback until then. */
class ChannelAvatarCache : public QObject {
	Q_OBJECT

public:
	static ChannelAvatarCache &Instance();

	/* Returns the cached pixmap when available, otherwise starts a download
	 * and returns a null pixmap. */
	QPixmap Avatar(const QString &channelId, const QString &url, int size);

signals:
	void avatarReady(const QString &channelId);

private:
	explicit ChannelAvatarCache(QObject *parent = nullptr);

	QString CacheFilePath(const QString &url) const;
	void Download(const QString &channelId, const QString &url);

	QNetworkAccessManager *netManager = nullptr;
	QHash<QString, QPixmap> memoryCache;
	QHash<QString, QString> inFlight;
	QString cacheDirectory;
};
