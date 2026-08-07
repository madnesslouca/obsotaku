/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "MultiStreamManager.hpp"

#include <string>
#include <vector>

/* Persists the destination list. Metadata (platform, name, server, track
 * selection) goes to the user configuration; stream keys never do — they live
 * in the operating-system credential store, keyed by channel id.
 *
 * OAuth channels keep only their account id here: the key is resolved from the
 * platform API right before going live. */
class MultistreamChannelStore {
public:
	/* Reads the stored list and migrates the old one-account-per-platform
	 * layout on first run. Stream keys of manual channels are loaded from
	 * the credential store; OAuth channels come back without credentials. */
	static std::vector<MultiStreamChannel> Load();
	static bool Save(const std::vector<MultiStreamChannel> &channels, std::string &error);

	static bool Upsert(const MultiStreamChannel &channel, std::string &error);
	static bool Remove(const std::string &channelId, std::string &error);
	static bool SetEnabled(const std::string &channelId, bool enabled, std::string &error);

	/* Writes back what a credential resolve learned about a channel: its
	 * display name, chat handle and avatar. Credentials are left out on
	 * purpose, so the stored channel keeps resolving them on demand. */
	static bool UpdateIdentity(const std::vector<MultiStreamChannel> &resolved, std::string &error);

	/* Stable id for a new manual destination on this platform. */
	static std::string NewManualChannelId(StreamPlatform platform);
	static std::string ChannelIdForAccount(StreamPlatform platform, const std::string &accountId);
};
