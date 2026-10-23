/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "MultiStreamManager.hpp"
#include "SecureTokenStore.hpp"

#include <functional>
#include <memory>

class QObject;

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
	struct Result {
		bool success = false;
		bool configCommitted = false;
		/* Durable success may outlive its profile/runtime. Apply channels only when
   * deliveryCurrent is true; a stale delivery is a warning, never a late rollback. */
		bool deliveryCurrent = false;
		bool rollbackComplete = true;
		SecureTokenStore::Status credentialStatus = SecureTokenStore::Status::NotStarted;
		std::vector<MultiStreamChannel> channels;
		std::string error;
		std::string warning;
	};
	using Completion = std::function<void(Result)>;
	using Request = std::shared_ptr<SecureTokenStore::RequestContext>;
	/* GUI entry points: metadata snapshot now, credentials on a serial expiring
  * worker, queued CAS/commit on GUI. Destroying receiver cancels the request. */
	static Request LoadAsync(QObject *receiver, Completion complete, std::function<bool()> canCommit = {});
	static Request SaveAsync(std::vector<MultiStreamChannel> channels, QObject *receiver, Completion complete,
				 std::function<bool()> canCommit = {});
	static Request UpsertAsync(MultiStreamChannel channel, QObject *receiver, Completion complete,
				   std::function<bool()> canCommit = {});
	static Request RemoveAsync(std::string channelId, QObject *receiver, Completion complete,
				   std::function<bool()> canCommit = {});
	/* Serial credential transaction for a local provider registration. guiCommit
  * runs only after backup/write and the same metadata/profile CAS. It must
  * restore its in-memory config fields if its durable save fails. */
	static Request SaveSecretAsync(std::string platformId, std::string accountId, std::string secret,
				       QObject *receiver, std::function<bool(std::string &)> guiCommit,
				       Completion complete, std::function<bool()> canCommit = {});
	/* Capture/invoke on GUI only; protects later OAuth/network delivery without
     * retaining config_t or extending a credential deadline over HTTP. */
	static std::function<bool()> CaptureMetadataGuard();
	static bool IsBusy();
	static void Shutdown();

	/* Reads the stored list, including the legacy account metadata. The next
     * successful save writes the new layout. Load never waits for the credential store: manual keys come from the
	 * scoped memory cache populated by LoadAsync. Missing/pending/error never
	 * changes the persisted enabled preference. OAuth has no stored ingest key. */
	static std::vector<MultiStreamChannel> Load();
	/* Metadata-only compatibility path. New/replaced/deleted manual credentials
  * require SaveAsync; this method never performs credential I/O. */
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
