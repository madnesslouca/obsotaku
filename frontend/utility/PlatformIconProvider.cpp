/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "PlatformIconProvider.hpp"
#include "StreamPlatformDisplay.hpp"
#include "platform.hpp"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace {
/* Simple Icons ship a single monochrome path with no fill attribute, so the
 * color is injected on the root element and inherited by the path. */
QByteArray TintedSvg(const QString &path, const QColor &color)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};

	QByteArray svg = file.readAll();
	const int insertAt = svg.indexOf("<svg");
	if (insertAt < 0)
		return {};

	svg.insert(insertAt + 4, QStringLiteral(" fill=\"%1\"").arg(color.name()).toUtf8());
	return svg;
}

QString ArtworkPath(StreamPlatform platform)
{
	std::string resolved;
	const std::string relative = "images/platforms/" + std::string(GetStreamPlatformInfo(platform).id) + ".svg";
	if (!GetDataFilePath(relative.c_str(), resolved))
		return {};
	return QString::fromStdString(resolved);
}

QPixmap MakePixmap(int size, qreal devicePixelRatio)
{
	QPixmap pixmap(qRound(size * devicePixelRatio), qRound(size * devicePixelRatio));
	pixmap.setDevicePixelRatio(devicePixelRatio);
	pixmap.fill(Qt::transparent);
	return pixmap;
}

void DrawFallbackGlyph(QPainter &painter, StreamPlatform platform, const QRectF &box, const QColor &color)
{
	QFont font = painter.font();
	font.setBold(true);
	font.setPixelSize(qRound(box.height() * 0.62));
	painter.setFont(font);
	painter.setPen(color);
	painter.drawText(box, Qt::AlignCenter, StreamPlatformDisplayName(platform).left(1).toUpper());
}
} // namespace

bool PlatformIconProvider::HasArtwork(StreamPlatform platform)
{
	static QHash<int, bool> cache;
	const int key = static_cast<int>(platform);
	const auto known = cache.constFind(key);
	if (known != cache.constEnd())
		return *known;

	const bool exists = !ArtworkPath(platform).isEmpty();
	cache.insert(key, exists);
	return exists;
}

QPixmap PlatformIconProvider::Glyph(StreamPlatform platform, int size, qreal devicePixelRatio)
{
	static QHash<QString, QPixmap> cache;
	const QString key = QStringLiteral("%1|%2|%3").arg(static_cast<int>(platform)).arg(size).arg(devicePixelRatio);
	const auto cached = cache.constFind(key);
	if (cached != cache.constEnd())
		return *cached;

	const QColor brand(StreamPlatformBrandColor(platform));
	QPixmap pixmap = MakePixmap(size, devicePixelRatio);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing, true);

	const QByteArray svg = TintedSvg(ArtworkPath(platform), brand);
	if (!svg.isEmpty()) {
		QSvgRenderer renderer(svg);
		if (renderer.isValid())
			renderer.render(&painter, QRectF(0, 0, size, size));
		else
			DrawFallbackGlyph(painter, platform, QRectF(0, 0, size, size), brand);
	} else {
		DrawFallbackGlyph(painter, platform, QRectF(0, 0, size, size), brand);
	}

	painter.end();
	cache.insert(key, pixmap);
	return pixmap;
}

QPixmap PlatformIconProvider::Badge(StreamPlatform platform, int size, qreal devicePixelRatio)
{
	static QHash<QString, QPixmap> cache;
	const QString key = QStringLiteral("%1|%2|%3").arg(static_cast<int>(platform)).arg(size).arg(devicePixelRatio);
	const auto cached = cache.constFind(key);
	if (cached != cache.constEnd())
		return *cached;

	const QColor brand(StreamPlatformBrandColor(platform));
	QPixmap pixmap = MakePixmap(size, devicePixelRatio);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing, true);

	QColor fill = brand;
	fill.setAlphaF(0.16f);
	QColor border = brand;
	border.setAlphaF(0.45f);

	const QRectF box(0.5, 0.5, size - 1.0, size - 1.0);
	painter.setBrush(fill);
	painter.setPen(QPen(border, 1.0));
	painter.drawRoundedRect(box, size * 0.26, size * 0.26);

	/* The glyph sits inside the badge with breathing room on every side. */
	const qreal inset = size * 0.26;
	const QRectF glyphBox = box.adjusted(inset / 2, inset / 2, -inset / 2, -inset / 2);
	const QByteArray svg = TintedSvg(ArtworkPath(platform), brand);
	if (!svg.isEmpty()) {
		QSvgRenderer renderer(svg);
		if (renderer.isValid())
			renderer.render(&painter, glyphBox);
		else
			DrawFallbackGlyph(painter, platform, box, brand);
	} else {
		DrawFallbackGlyph(painter, platform, box, brand);
	}

	painter.end();
	cache.insert(key, pixmap);
	return pixmap;
}
