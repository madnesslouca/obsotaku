/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QPixmap>
#include <QString>

/* Artwork for the role badges shown next to a nickname in chat.
 *
 * Files live in data/images/chat-badges, named after the short role label the
 * aggregator produces, lowercased: host, mod, vip, sub. Unlike the platform
 * logos these arrive fully coloured, so nothing is tinted here. A label with no
 * file returns a null pixmap and the caller falls back to the text pill. */
class ChatBadgeIcons {
public:
	static QPixmap Glyph(const QString &roleLabel, int size, qreal devicePixelRatio = 2.0);
};
