/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChannelAvatarCache.hpp"

#include <QPixmap>

#include <util/base.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QUrl>

#include "moc_ChannelAvatarCache.cpp"

namespace {
constexpr qint64 MAX_AVATAR_BYTES = 2 * 1024 * 1024;

/* Circular crop so every card looks the same regardless of what the platform
 * returned (square, wide banner, transparent PNG). */
QPixmap RoundedAvatar(const QImage &image, int size)
{
	if (image.isNull() || size <= 0)
		return {};

	const QImage scaled =
		image.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	const int x = (scaled.width() - size) / 2;
	const int y = (scaled.height() - size) / 2;

	QPixmap result(size, size);
	result.fill(Qt::transparent);

	QPainter painter(&result);
	painter.setRenderHint(QPainter::Antialiasing, true);
	QPainterPath clip;
	clip.addEllipse(0, 0, size, size);
	painter.setClipPath(clip);
	painter.drawImage(QPoint(0, 0), scaled, QRect(x, y, size, size));
	return result;
}
} // namespace

ChannelAvatarCache &ChannelAvatarCache::Instance()
{
	static ChannelAvatarCache cache;
	return cache;
}

ChannelAvatarCache::ChannelAvatarCache(QObject *parent) : QObject(parent)
{
	netManager = new QNetworkAccessManager(this);
	cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
			 QStringLiteral("/multistream-avatars");
	QDir().mkpath(cacheDirectory);
}

QString ChannelAvatarCache::CacheFilePath(const QString &url) const
{
	/* Name the file after the URL, not the channel, so a changed picture
	 * downloads instead of serving the stale one forever. */
	const QByteArray digest = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex();
	return cacheDirectory + QStringLiteral("/") + QString::fromLatin1(digest.left(32)) +
	       QStringLiteral(".png");
}

QPixmap ChannelAvatarCache::Avatar(const QString &channelId, const QString &url, int size)
{
	if (url.isEmpty() || channelId.isEmpty())
		return {};

	const QString key = QStringLiteral("%1|%2").arg(url).arg(size);
	const auto cached = memoryCache.constFind(key);
	if (cached != memoryCache.constEnd())
		return *cached;

	const QString path = CacheFilePath(url);
	QImage image;
	if (QFile::exists(path) && image.load(path)) {
		const QPixmap pixmap = RoundedAvatar(image, size);
		memoryCache.insert(key, pixmap);
		return pixmap;
	}

	Download(channelId, url);
	return {};
}

void ChannelAvatarCache::Download(const QString &channelId, const QString &url)
{
	if (inFlight.contains(url))
		return;

	const QUrl parsed(url);
	/* Profile pictures come from the platform APIs over HTTPS; anything else
	 * is not worth fetching. */
	if (!parsed.isValid() || parsed.scheme() != QStringLiteral("https"))
		return;

	inFlight.insert(url, channelId);
	QNetworkRequest request(parsed);
	request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OBS-Multistream/0.1"));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

	QNetworkReply *reply = netManager->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, url, channelId]() {
		reply->deleteLater();
		inFlight.remove(url);

		if (reply->error() != QNetworkReply::NoError)
			return;
		const QByteArray data = reply->readAll();
		if (data.isEmpty() || data.size() > MAX_AVATAR_BYTES)
			return;

		QImage image;
		if (!image.loadFromData(data))
			return;

		/* Store normalized: the platforms serve wildly different sizes and
		 * we only ever draw small circles. */
		image = image.scaled(128, 128, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
		if (!image.save(CacheFilePath(url), "PNG"))
			blog(LOG_DEBUG, "Could not cache a channel avatar on disk");

		emit avatarReady(channelId);
	});
}
