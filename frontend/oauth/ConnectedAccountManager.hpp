/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "PlatformOAuthClient.hpp"

#include <utility/MultiStreamManager.hpp>

#include <string>

struct ConnectedStreamAccount {
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	std::string accountId;
	std::string displayName;
	bool enabled = true;
};

class ConnectedAccountManager {
public:
	static bool CompleteConnection(StreamPlatform platform, const OAuthClientRegistration &registration,
				       const OAuthTokenSet &tokens, ConnectedStreamAccount &account,
				       MultiStreamChannel &channel, std::string &error);
	static bool ResolveChannel(const ConnectedStreamAccount &account, const OAuthClientRegistration &registration,
				   const std::string &redirectUri, MultiStreamChannel &channel, std::string &error);
	static bool Disconnect(const ConnectedStreamAccount &account, std::string &error);

	/* ResolveChannel rebuilds a channel from what the platform returns, which
	 * knows nothing about the user's own settings. This copies those back, so
	 * resolving credentials never silently resets the audio track, the VOD
	 * track, the broadcast metadata or the enabled state. */
	static void PreserveUserSettings(const MultiStreamChannel &stored, MultiStreamChannel &resolved);
};
