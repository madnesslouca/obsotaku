/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ConnectedAccountManager.hpp"

using namespace std;

static MultiStreamChannel MakeChannel(const ConnectedStreamAccount &account, const ResolvedStreamIngest &ingest)
{
	const auto &platform = GetStreamPlatformInfo(account.platform);
	MultiStreamChannel channel;
	channel.id = string(platform.id) + ":" + account.accountId;
	channel.displayName = account.displayName.empty() ? string(platform.displayName) : account.displayName;
	channel.platform = account.platform;
	channel.accountId = account.accountId;
	channel.server = ingest.server;
	channel.streamKey = ingest.streamKey;
	channel.avatarUrl = ingest.avatarUrl;
	channel.enabled = account.enabled;
	return channel;
}

bool ConnectedAccountManager::CompleteConnection(StreamPlatform platform,
					  const OAuthClientRegistration &registration, const OAuthTokenSet &tokens,
					  ConnectedStreamAccount &account, MultiStreamChannel &channel,
					  string &error)
{
	ResolvedStreamIngest ingest;
	if (!PlatformOAuthClient::ResolveIngest(platform, registration, tokens, ingest, error))
		return false;
	if (ingest.accountId.empty()) {
		error = "The connected platform returned no stable account id.";
		return false;
	}

	ConnectedStreamAccount connected{platform, ingest.accountId, ingest.displayName, true};
	if (!tokens.Save(platform, connected.accountId, error))
		return false;

	channel = MakeChannel(connected, ingest);
	account = std::move(connected);
	error.clear();
	return true;
}

bool ConnectedAccountManager::ResolveChannel(const ConnectedStreamAccount &account,
					     const OAuthClientRegistration &registration, const string &redirectUri,
					     MultiStreamChannel &channel, string &error)
{
	if (account.accountId.empty() || account.platform == StreamPlatform::CustomRtmp) {
		error = "A connected OAuth account is required to resolve a stream channel.";
		return false;
	}

	auto storedTokens = OAuthTokenSet::Load(account.platform, account.accountId, error);
	if (!storedTokens) {
		if (error.empty())
			error = "No stored OAuth credential exists for this account.";
		return false;
	}

	if (storedTokens->AccessTokenExpired()) {
		OAuthTokenSet refreshedTokens;
		if (!PlatformOAuthClient::RefreshTokens(account.platform, registration, redirectUri, *storedTokens,
						     refreshedTokens, error))
			return false;
		if (!refreshedTokens.Save(account.platform, account.accountId, error))
			return false;
		*storedTokens = std::move(refreshedTokens);
	}

	ResolvedStreamIngest ingest;
	if (!PlatformOAuthClient::ResolveIngest(account.platform, registration, *storedTokens, ingest, error))
		return false;
	if (!ingest.accountId.empty() && ingest.accountId != account.accountId) {
		error = "The OAuth credential resolved to a different platform account.";
		return false;
	}

	channel = MakeChannel(account, ingest);
	error.clear();
	return true;
}

bool ConnectedAccountManager::Disconnect(const ConnectedStreamAccount &account, string &error)
{
	if (account.accountId.empty() || account.platform == StreamPlatform::CustomRtmp) {
		error = "A connected OAuth account is required to remove credentials.";
		return false;
	}
	return OAuthTokenSet::Remove(account.platform, account.accountId, error);
}
