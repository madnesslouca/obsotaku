/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "StreamPlatform.hpp"

#include <QPixmap>

/* Platform artwork for the channel cards and the add-channel grid.
 *
 * Logos live in data/images/platforms/<platform-id>.svg and are tinted with
 * the catalog brand color at render time. A platform without a file falls back
 * to its initial drawn over the same brand color, so the interface never has a
 * hole when artwork is missing. */
class PlatformIconProvider {
public:
	/* Rounded brand badge: tinted background plus the platform glyph. */
	static QPixmap Badge(StreamPlatform platform, int size, qreal devicePixelRatio = 2.0);

	/* Bare glyph with no background, for places that already have one. */
	static QPixmap Glyph(StreamPlatform platform, int size, qreal devicePixelRatio = 2.0);

	static bool HasArtwork(StreamPlatform platform);
};
