/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "OAuthTokenSet.hpp"

#include <utility/StreamPlatform.hpp>

#include <cstdint>
#include <string>

struct OAuthClientRegistration {
	std::string clientId;
	// Production builds should point Kick at a backend that adds the client
	// secret. Personal development builds may load it from the Windows
	// Credential Manager instead; it is never compiled into the binary.
	std::string tokenExchangeEndpoint;
	std::string clientSecret;
};

struct OAuthAuthorizationSession {
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	std::string authorizationUrl;
	std::string redirectUri;
	std::string state;
	std::string codeVerifier;
};

struct OAuthDeviceAuthorization {
	std::string deviceCode;
	std::string userCode;
	std::string verificationUri;
	int expiresInSeconds = 0;
	int pollIntervalSeconds = 5;
};

enum class OAuthDevicePollStatus {
	Authorized,
	Pending,
	SlowDown,
	Expired,
	Denied,
	Error,
};

struct ResolvedStreamIngest {
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	std::string accountId;
	std::string displayName;
	std::string server;
	std::string streamKey;
	/* Profile picture of the connected channel, when the platform returns
	 * one. Empty is normal and the card falls back to the initial. */
	std::string avatarUrl;
};

class PlatformOAuthClient {
public:
	static bool CreateAuthorizationSession(StreamPlatform platform, const OAuthClientRegistration &registration,
					       const std::string &redirectUri, OAuthAuthorizationSession &session,
					       std::string &error);
	static bool ExchangeAuthorizationCode(const OAuthClientRegistration &registration,
					      const OAuthAuthorizationSession &session, const std::string &code,
					      OAuthTokenSet &tokens, std::string &error);

	static bool StartDeviceAuthorization(StreamPlatform platform, const OAuthClientRegistration &registration,
					     OAuthDeviceAuthorization &authorization, std::string &error);
	static OAuthDevicePollStatus PollDeviceAuthorization(StreamPlatform platform,
							     const OAuthClientRegistration &registration,
							     const OAuthDeviceAuthorization &authorization,
							     OAuthTokenSet &tokens, std::string &error);

	static bool RefreshTokens(StreamPlatform platform, const OAuthClientRegistration &registration,
				  const std::string &redirectUri, const OAuthTokenSet &currentTokens,
				  OAuthTokenSet &tokens, std::string &error);
	static bool ResolveIngest(StreamPlatform platform, const OAuthClientRegistration &registration,
				  const OAuthTokenSet &tokens, ResolvedStreamIngest &ingest, std::string &error);
};
