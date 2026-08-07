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
	/* The bare platform name is the last resort: a channel labelled "Kick"
	 * tells the user nothing when several accounts share the platform. */
	if (!account.displayName.empty() && account.displayName != platform.displayName)
		channel.displayName = account.displayName;
	else if (!ingest.displayName.empty())
		channel.displayName = ingest.displayName;
	else
		channel.displayName = string(platform.displayName);
	channel.platform = account.platform;
	channel.accountId = account.accountId;
	/* Always the handle the platform reports, never the editable label. */
	channel.chatAddress = ingest.displayName;
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

void ConnectedAccountManager::PreserveUserSettings(const MultiStreamChannel &stored, MultiStreamChannel &resolved)
{
	if (!stored.id.empty())
		resolved.id = stored.id;
	/* A name the user edited outranks whatever the platform reports. The bare
	 * platform name is not one of those: it is the placeholder a channel gets
	 * when nothing was stored, and keeping it would freeze the label there. */
	if (!stored.displayName.empty() && stored.displayName != GetStreamPlatformInfo(stored.platform).displayName)
		resolved.displayName = stored.displayName;
	resolved.audioMixIndex = stored.audioMixIndex;
	resolved.vodTrackEnabled = stored.vodTrackEnabled;
	resolved.vodTrackIndex = stored.vodTrackIndex;
	resolved.enabled = stored.enabled;
	resolved.title = stored.title;
	resolved.categoryId = stored.categoryId;
	resolved.categoryName = stored.categoryName;
	/* Keep the cached avatar when the platform did not return one. */
	if (resolved.avatarUrl.empty())
		resolved.avatarUrl = stored.avatarUrl;
	/* The chat handle belongs to the platform, so a fresh one always wins;
	 * the stored one is only a fallback for a reply that omitted it. */
	if (resolved.chatAddress.empty())
		resolved.chatAddress = stored.chatAddress;
}

bool ConnectedAccountManager::Disconnect(const ConnectedStreamAccount &account, string &error)
{
	if (account.accountId.empty() || account.platform == StreamPlatform::CustomRtmp) {
		error = "A connected OAuth account is required to remove credentials.";
		return false;
	}
	return OAuthTokenSet::Remove(account.platform, account.accountId, error);
}
