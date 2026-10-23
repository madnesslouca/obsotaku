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
#include <utility/SecureTokenStore.hpp>

#include <atomic>
#include <string>

struct ConnectedStreamAccount {
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	std::string accountId;
	std::string displayName;
	bool enabled = true;
};

class ConnectedAccountManager {
public:
	/* Worker-thread lease: backup and mutations remain serialized with OAuth
	 * refresh until Commit/Rollback and destruction on that SAME worker. Never
	 * pass the lease to the GUI thread or let a GUI callback destroy it. */
	class CredentialRemoval {
	public:
		~CredentialRemoval();
		CredentialRemoval(const CredentialRemoval &) = delete;
		CredentialRemoval &operator=(const CredentialRemoval &) = delete;
		bool Remove(std::string &error);
		/* Caller installs a NEW, uncancelled RequestScope with one shared
		 * recovery deadline for the entire transaction. Failure is explicit;
		 * destruction does not silently retry credential I/O. */
		bool Rollback(std::string &error);
		void Commit() noexcept;

	private:
		friend class ConnectedAccountManager;
		struct Impl;
		explicit CredentialRemoval(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> impl;
	};

	/* Blocking WORKER API. Cooperatively waits for the same per-account mutex
	 * as refresh, then backs up the current raw credential without deleting it.
	 * Cancellation/expiry while waiting never calls the credential backend. */
	static std::unique_ptr<CredentialRemoval>
	BeginCredentialRemoval(const ConnectedStreamAccount &account,
			       std::shared_ptr<SecureTokenStore::RequestContext> request, std::string &error);

	/* Loads the current credential and refreshes it when needed. Refreshes are
	 * serialized per account because some platforms rotate refresh tokens and
	 * invalidate the old value as soon as it is used. */
	static bool LoadUsableTokens(StreamPlatform platform, const std::string &accountId,
				     const OAuthClientRegistration &registration, const std::string &redirectUri,
				     OAuthTokenSet &tokens, std::string &error);

	static bool CompleteConnection(StreamPlatform platform, const OAuthClientRegistration &registration,
				       const OAuthTokenSet &tokens, ConnectedStreamAccount &account,
				       MultiStreamChannel &channel, std::string &error,
				       const std::atomic_bool *canceled = nullptr);
	static bool ResolveChannel(const ConnectedStreamAccount &account, const OAuthClientRegistration &registration,
				   const std::string &redirectUri, MultiStreamChannel &channel, std::string &error);
	/* Blocking compatibility wrapper; UI callers use the lease on a worker. */
	static bool Disconnect(const ConnectedStreamAccount &account, std::string &error);

	/* ResolveChannel rebuilds a channel from what the platform returns, which
	 * knows nothing about the user's own settings. This copies those back, so
	 * resolving credentials never silently resets the audio track, the VOD
	 * track, the broadcast metadata or the enabled state. */
	static void PreserveUserSettings(const MultiStreamChannel &stored, MultiStreamChannel &resolved);
};
