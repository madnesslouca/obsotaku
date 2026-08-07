/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "StreamPlatform.hpp"

#include <OBSApp.hpp>

#include <QString>

/* User-interface helpers for the platform catalog. Kept out of
 * StreamPlatform.cpp so that file stays free of Qt and the application
 * singleton, which lets the product tests compile it on its own. */

inline QString StreamPlatformDisplayName(StreamPlatform platform)
{
	/* Brand names are shown as they are; only the generic entry is
	 * translated, otherwise it reads as English inside a localized list. */
	if (platform == StreamPlatform::CustomRtmp)
		return QTStr("Multistream.Platform.CustomRtmp");
	const auto name = GetStreamPlatformInfo(platform).displayName;
	return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

inline QString StreamPlatformId(StreamPlatform platform)
{
	const auto id = GetStreamPlatformInfo(platform).id;
	return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

inline QString StreamPlatformBrandColor(StreamPlatform platform)
{
	const auto color = GetStreamPlatformInfo(platform).brandColor;
	return QString::fromUtf8(color.data(), static_cast<qsizetype>(color.size()));
}

/* Artwork lives in PlatformIconProvider, which needs file access and a cache. */
