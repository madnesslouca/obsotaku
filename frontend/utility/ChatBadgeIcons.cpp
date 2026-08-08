/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatBadgeIcons.hpp"
#include "platform.hpp"

#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace {
/* PNG is tried first so a hand-made badge can replace a drawn one by dropping
 * the file in, with no code change. */
QString ArtworkPath(const QString &roleLabel)
{
	for (const char *extension : {".png", ".svg"}) {
		const std::string relative = "images/chat-badges/" + roleLabel.toLower().toStdString() + extension;
		std::string resolved;
		if (GetDataFilePath(relative.c_str(), resolved))
			return QString::fromStdString(resolved);
	}
	return {};
}
} // namespace

QPixmap ChatBadgeIcons::Glyph(const QString &roleLabel, int size, qreal devicePixelRatio)
{
	static QHash<QString, QPixmap> cache;
	const QString key = QStringLiteral("%1|%2|%3").arg(roleLabel.toLower()).arg(size).arg(devicePixelRatio);
	const auto cached = cache.constFind(key);
	if (cached != cache.constEnd())
		return *cached;

	const QString path = ArtworkPath(roleLabel);
	QPixmap pixmap;

	if (path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
		QSvgRenderer renderer(path);
		if (renderer.isValid()) {
			pixmap = QPixmap(qRound(size * devicePixelRatio), qRound(size * devicePixelRatio));
			pixmap.setDevicePixelRatio(devicePixelRatio);
			pixmap.fill(Qt::transparent);
			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			renderer.render(&painter, QRectF(0, 0, size, size));
		}
	} else if (!path.isEmpty()) {
		QPixmap source(path);
		if (!source.isNull()) {
			pixmap = source.scaled(qRound(size * devicePixelRatio), qRound(size * devicePixelRatio),
					       Qt::KeepAspectRatio, Qt::SmoothTransformation);
			pixmap.setDevicePixelRatio(devicePixelRatio);
		}
	}

	cache.insert(key, pixmap);
	return pixmap;
}
