/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ConnectedAccountManager.hpp"

#include <map>
#include <memory>
#include <mutex>

using namespace std;

namespace {
mutex accountMutexesMutex;
map<string, weak_ptr<mutex>> accountMutexes;

shared_ptr<mutex> MutexForAccount(StreamPlatform platform, const string &accountId)
{
	const string key = string(GetStreamPlatformInfo(platform).id) + ':' + accountId;
	lock_guard lock(accountMutexesMutex);
	auto &slot = accountMutexes[key];
	auto result = slot.lock();
	if (!result) {
		result = make_shared<mutex>();
		slot = result;
	}
	return result;
}
} // namespace

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

bool ConnectedAccountManager::LoadUsableTokens(StreamPlatform platform, const string &accountId,
					       const OAuthClientRegistration &registration, const string &redirectUri,
					       OAuthTokenSet &tokens, string &error)
{
	if (accountId.empty()) {
		error = "An account id is required to load OAuth credentials.";
		return false;
	}

	/* Load again after entering the account lock. Another component may have
	 * refreshed and rotated the token while this caller was waiting. */
	const auto accountMutex = MutexForAccount(platform, accountId);
	lock_guard accountLock(*accountMutex);
	auto stored = OAuthTokenSet::Load(platform, accountId, error);
	if (!stored) {
		if (error.empty()) {
			error = "No stored OAuth credential exists for this account.";
		}
		return false;
	}

	if (stored->AccessTokenExpired()) {
		OAuthTokenSet refreshed;
		if (!PlatformOAuthClient::RefreshTokens(platform, registration, redirectUri, *stored, refreshed,
							error)) {
			return false;
		}
		/* A rotated refresh token is not usable until it is durable. Continuing
		 * after a failed save would leave the account broken on next launch. */
		if (!refreshed.Save(platform, accountId, error)) {
			return false;
		}
		*stored = std::move(refreshed);
	}

	tokens = std::move(*stored);
	error.clear();
	return true;
}

bool ConnectedAccountManager::CompleteConnection(StreamPlatform platform, const OAuthClientRegistration &registration,
						 const OAuthTokenSet &tokens, ConnectedStreamAccount &account,
						 MultiStreamChannel &channel, string &error,
						 const atomic_bool *canceled)
{
	if (canceled && canceled->load()) {
		error = "The account connection was canceled.";
		return false;
	}
	ResolvedStreamIngest ingest;
	if (!PlatformOAuthClient::ResolveIngest(platform, registration, tokens, ingest, error))
		return false;
	if (canceled && canceled->load()) {
		error = "The account connection was canceled.";
		return false;
	}
	if (ingest.accountId.empty()) {
		error = "The connected platform returned no stable account id.";
		return false;
	}

	ConnectedStreamAccount connected{platform, ingest.accountId, ingest.displayName, true};
	const auto accountMutex = MutexForAccount(platform, connected.accountId);
	lock_guard accountLock(*accountMutex);
	if (canceled && canceled->load()) {
		error = "The account connection was canceled.";
		return false;
	}
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

	OAuthTokenSet storedTokens;
	if (!LoadUsableTokens(account.platform, account.accountId, registration, redirectUri, storedTokens, error)) {
		return false;
	}

	ResolvedStreamIngest ingest;
	if (!PlatformOAuthClient::ResolveIngest(account.platform, registration, storedTokens, ingest, error)) {
		return false;
	}
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
	const auto accountMutex = MutexForAccount(account.platform, account.accountId);
	lock_guard accountLock(*accountMutex);
	return OAuthTokenSet::Remove(account.platform, account.accountId, error);
}
