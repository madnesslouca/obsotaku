/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ConnectedAccountManager.hpp"

#include <util/base.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

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

struct ConnectedAccountManager::CredentialRemoval::Impl {
	Impl(const ConnectedStreamAccount &account, shared_ptr<SecureTokenStore::RequestContext> request)
		: platform(account.platform), accountId(account.accountId), request(std::move(request)),
		  accountMutex(MutexForAccount(platform, accountId)), accountLock(*accountMutex, defer_lock),
		  owner(this_thread::get_id())
	{
	}
	~Impl()
	{
		assert(owner == this_thread::get_id());
		ClearBackup();
		if (accountLock.owns_lock())
			accountLock.unlock();
		/* Recovery is explicit: no credential I/O from a destructor, and no
		 * remote text, account identifier or token in this diagnostic. */
		if (attempted)
			blog(LOG_ERROR, "OAuth credential removal ended without confirmed commit or rollback.");
	}
	void ClearBackup() noexcept
	{
		if (!backup)
			return;
		volatile char *bytes = backup->empty() ? nullptr : backup->data();
		for (size_t index = 0; index < backup->size(); ++index)
			bytes[index] = 0;
		backup.reset();
	}
	bool MayContinue(string &error) const
	{
		if (request->IsCancelled()) {
			error = "OAuth credential removal was cancelled.";
			return false;
		}
		if (chrono::steady_clock::now() >= request->GetDeadline()) {
			error = "OAuth credential removal timed out.";
			return false;
		}
		return true;
	}
	const StreamPlatform platform;
	const string accountId;
	const shared_ptr<SecureTokenStore::RequestContext> request;
	const shared_ptr<mutex> accountMutex;
	unique_lock<mutex> accountLock;
	const thread::id owner;
	optional<string> backup;
	bool attempted = false;
	bool committed = false;
};

ConnectedAccountManager::CredentialRemoval::CredentialRemoval(unique_ptr<Impl> impl) : impl(std::move(impl)) {}

ConnectedAccountManager::CredentialRemoval::~CredentialRemoval() = default;

unique_ptr<ConnectedAccountManager::CredentialRemoval>
ConnectedAccountManager::BeginCredentialRemoval(const ConnectedStreamAccount &account,
						shared_ptr<SecureTokenStore::RequestContext> request, string &error)
{
	if (account.accountId.empty() || account.platform == StreamPlatform::CustomRtmp) {
		error = "A connected OAuth account is required to remove credentials.";
		return {};
	}
	if (!request)
		request = make_shared<SecureTokenStore::RequestContext>();
	auto state = make_unique<CredentialRemoval::Impl>(account, std::move(request));
	while (state->MayContinue(error)) {
		if (state->accountLock.try_lock()) {
			if (!state->MayContinue(error))
				return {};
			SecureTokenStore::RequestScope scope(state->request);
			state->backup = SecureTokenStore::Load(string(GetStreamPlatformInfo(account.platform).id),
						     account.accountId, error);
			if (!error.empty())
				return {};
			error.clear();
			return unique_ptr<CredentialRemoval>(new CredentialRemoval(std::move(state)));
		}
		this_thread::sleep_until(min(state->request->GetDeadline(),
					    chrono::steady_clock::now() + chrono::milliseconds(10)));
	}
	return {};
}

bool ConnectedAccountManager::CredentialRemoval::Remove(string &error)
{
	assert(impl->owner == this_thread::get_id());
	if (impl->committed) {
		error = "OAuth credential removal was already committed.";
		return false;
	}
	if (!impl->MayContinue(error))
		return false;
	SecureTokenStore::RequestScope scope(impl->request);
	/* Cancellation may arrive after the service commits but before its reply. */
	impl->attempted = true;
	return OAuthTokenSet::Remove(impl->platform, impl->accountId, error);
}

bool ConnectedAccountManager::CredentialRemoval::Rollback(string &error)
{
	assert(impl->owner == this_thread::get_id());
	if (impl->committed) {
		error = "A committed OAuth credential removal cannot be rolled back.";
		return false;
	}
	if (!impl->attempted) {
		error.clear();
		return true;
	}
	/* Deliberately inherit the caller's fresh recovery RequestScope. Reusing
	 * impl->request would make recovery impossible after cancellation/expiry. */
	const string name = string(GetStreamPlatformInfo(impl->platform).id);
	const bool restored = impl->backup ? SecureTokenStore::Save(name, impl->accountId, *impl->backup, error)
					   : SecureTokenStore::Remove(name, impl->accountId, error);
	if (restored)
		impl->attempted = false;
	return restored;
}

void ConnectedAccountManager::CredentialRemoval::Commit() noexcept
{
	assert(impl->owner == this_thread::get_id());
	impl->attempted = false;
	impl->committed = true;
	impl->ClearBackup();
}

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
	resolved.videoLayout = stored.videoLayout;
	resolved.portraitFit = stored.portraitFit;
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
	auto lease = BeginCredentialRemoval(account, make_shared<SecureTokenStore::RequestContext>(), error);
	if (!lease)
		return false;
	if (lease->Remove(error)) {
		lease->Commit();
		return true;
	}
	const string removalError = error;
	SecureTokenStore::RequestScope recovery(make_shared<SecureTokenStore::RequestContext>());
	string rollbackError;
	const bool restored = lease->Rollback(rollbackError);
	error = removalError;
	if (!restored)
		error += " Restoring the previous OAuth credential did not complete.";
	return false;
}
