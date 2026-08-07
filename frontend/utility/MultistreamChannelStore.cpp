/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamChannelStore.hpp"
#include "SecureTokenStore.hpp"

#include <OBSApp.hpp>

#include <util/base.h>
#include <util/config-file.h>

#include <QUuid>

#include <algorithm>

using namespace std;

namespace {
constexpr const char *SECTION = "MultistreamChannels";
constexpr const char *MANUAL_KEY_STORE = "manual-rtmp";
constexpr int MAX_STORED_CHANNELS = 32;

string Key(int index, const char *name)
{
	return "Channel" + to_string(index) + "." + name;
}

const char *StringValue(config_t *config, const string &key)
{
	const char *value = config_get_string(config, SECTION, key.c_str());
	return value ? value : "";
}

size_t ClampTrack(int64_t value, size_t fallback)
{
	return (value < 0 || value >= MAX_AUDIO_MIXES) ? fallback : static_cast<size_t>(value);
}

/* Reads the pre-store layout, where each platform had one hard-coded section.
 * Runs once: Save() writes the new layout and the old keys stop being read. */
vector<MultiStreamChannel> LoadLegacyAccounts(config_t *config)
{
	vector<MultiStreamChannel> channels;
	for (const auto platform : {StreamPlatform::YouTube, StreamPlatform::Twitch, StreamPlatform::Kick}) {
		const string section = StreamPlatformConfigSection(platform);
		const char *accountId = config_get_string(config, section.c_str(), "AccountId");
		if (!accountId || !*accountId)
			continue;

		const char *displayName = config_get_string(config, section.c_str(), "DisplayName");
		const auto &info = GetStreamPlatformInfo(platform);

		MultiStreamChannel channel;
		channel.id = MultistreamChannelStore::ChannelIdForAccount(platform, accountId);
		channel.displayName = displayName && *displayName ? displayName : string(info.displayName);
		channel.platform = platform;
		channel.accountId = accountId;
		channel.audioMixIndex = ClampTrack(config_get_int(config, section.c_str(), "AudioTrack"), 0);
		channel.vodTrackEnabled = config_get_bool(config, section.c_str(), "VodTrackEnabled");
		channel.vodTrackIndex = ClampTrack(config_get_int(config, section.c_str(), "VodTrackIndex"), 1);
		channel.enabled = config_has_user_value(config, section.c_str(), "ChannelEnabled")
					  ? config_get_bool(config, section.c_str(), "ChannelEnabled")
					  : true;
		channels.emplace_back(std::move(channel));
	}
	return channels;
}
} // namespace

string MultistreamChannelStore::ChannelIdForAccount(StreamPlatform platform, const string &accountId)
{
	return string(GetStreamPlatformInfo(platform).id) + ":" + accountId;
}

string MultistreamChannelStore::NewManualChannelId(StreamPlatform platform)
{
	const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
	return string(GetStreamPlatformInfo(platform).id) + ":manual:" + uuid.toStdString();
}

vector<MultiStreamChannel> MultistreamChannelStore::Load()
{
	config_t *config = App()->GetUserConfig();
	if (!config)
		return {};

	if (!config_has_user_value(config, SECTION, "Count"))
		return LoadLegacyAccounts(config);

	const int count = static_cast<int>(config_get_int(config, SECTION, "Count"));
	vector<MultiStreamChannel> channels;
	for (int index = 0; index < min(count, MAX_STORED_CHANNELS); ++index) {
		const string id = StringValue(config, Key(index, "Id"));
		const auto platform = StreamPlatformFromId(StringValue(config, Key(index, "Platform")));
		if (id.empty() || !platform)
			continue;

		MultiStreamChannel channel;
		channel.id = id;
		channel.platform = *platform;
		channel.displayName = StringValue(config, Key(index, "DisplayName"));
		channel.accountId = StringValue(config, Key(index, "AccountId"));
		channel.server = StringValue(config, Key(index, "Server"));
		channel.avatarUrl = StringValue(config, Key(index, "AvatarUrl"));
		channel.title = StringValue(config, Key(index, "Title"));
		channel.categoryId = StringValue(config, Key(index, "CategoryId"));
		channel.categoryName = StringValue(config, Key(index, "CategoryName"));
		channel.audioMixIndex = ClampTrack(config_get_int(config, SECTION, Key(index, "AudioTrack").c_str()), 0);
		channel.vodTrackEnabled = config_get_bool(config, SECTION, Key(index, "VodTrackEnabled").c_str());
		channel.vodTrackIndex =
			ClampTrack(config_get_int(config, SECTION, Key(index, "VodTrackIndex").c_str()), 1);
		channel.enabled = config_get_bool(config, SECTION, Key(index, "Enabled").c_str());
		if (channel.displayName.empty())
			channel.displayName = string(GetStreamPlatformInfo(*platform).displayName);

		if (GetStreamPlatformInfo(*platform).ingestMode == StreamIngestMode::ManualStreamKey) {
			string error;
			auto key = SecureTokenStore::Load(MANUAL_KEY_STORE, channel.id, error);
			if (key) {
				channel.streamKey = std::move(*key);
			} else {
				/* Without its key the destination cannot go live, so
				 * it comes back disabled rather than failing at start. */
				channel.enabled = false;
				if (!error.empty())
					blog(LOG_WARNING, "Could not read the stored stream key for %s: %s",
					     channel.displayName.c_str(), error.c_str());
			}
		}

		channels.emplace_back(std::move(channel));
	}
	return channels;
}

bool MultistreamChannelStore::Save(const vector<MultiStreamChannel> &channels, string &error)
{
	config_t *config = App()->GetUserConfig();
	if (!config) {
		error = "The user configuration is not available.";
		return false;
	}
	if (channels.size() > MAX_STORED_CHANNELS) {
		error = "Too many multistream destinations.";
		return false;
	}

	/* Clear the previous list first: shrinking it would otherwise leave stale
	 * entries that Load() would read back. */
	const int previousCount = static_cast<int>(config_get_int(config, SECTION, "Count"));
	for (int index = 0; index < min(previousCount, MAX_STORED_CHANNELS); ++index) {
		for (const char *name : {"Id", "Platform", "DisplayName", "AccountId", "Server", "AvatarUrl", "Title",
					 "CategoryId", "CategoryName", "AudioTrack", "VodTrackEnabled",
					 "VodTrackIndex", "Enabled"})
			config_remove_value(config, SECTION, Key(index, name).c_str());
	}

	int index = 0;
	for (const auto &channel : channels) {
		const auto &info = GetStreamPlatformInfo(channel.platform);
		config_set_string(config, SECTION, Key(index, "Id").c_str(), channel.id.c_str());
		config_set_string(config, SECTION, Key(index, "Platform").c_str(), string(info.id).c_str());
		config_set_string(config, SECTION, Key(index, "DisplayName").c_str(), channel.displayName.c_str());
		config_set_string(config, SECTION, Key(index, "AccountId").c_str(), channel.accountId.c_str());
		config_set_string(config, SECTION, Key(index, "Server").c_str(), channel.server.c_str());
		config_set_string(config, SECTION, Key(index, "AvatarUrl").c_str(), channel.avatarUrl.c_str());
		config_set_string(config, SECTION, Key(index, "Title").c_str(), channel.title.c_str());
		config_set_string(config, SECTION, Key(index, "CategoryId").c_str(), channel.categoryId.c_str());
		config_set_string(config, SECTION, Key(index, "CategoryName").c_str(), channel.categoryName.c_str());
		config_set_int(config, SECTION, Key(index, "AudioTrack").c_str(),
			       static_cast<int64_t>(channel.audioMixIndex));
		config_set_bool(config, SECTION, Key(index, "VodTrackEnabled").c_str(), channel.vodTrackEnabled);
		config_set_int(config, SECTION, Key(index, "VodTrackIndex").c_str(),
			       static_cast<int64_t>(channel.vodTrackIndex));
		config_set_bool(config, SECTION, Key(index, "Enabled").c_str(), channel.enabled);

		/* Stream keys must never reach the .ini. */
		if (info.ingestMode == StreamIngestMode::ManualStreamKey && !channel.streamKey.empty()) {
			string keyError;
			if (!SecureTokenStore::Save(MANUAL_KEY_STORE, channel.id, channel.streamKey, keyError)) {
				error = keyError;
				return false;
			}
		}
		++index;
	}

	config_set_int(config, SECTION, "Count", index);
	config_save_safe(config, "tmp", nullptr);
	error.clear();
	return true;
}

bool MultistreamChannelStore::Upsert(const MultiStreamChannel &channel, string &error)
{
	if (channel.id.empty()) {
		error = "A multistream destination needs an id.";
		return false;
	}

	auto channels = Load();
	auto existing = find_if(channels.begin(), channels.end(),
				[&](const MultiStreamChannel &item) { return item.id == channel.id; });
	if (existing != channels.end())
		*existing = channel;
	else
		channels.push_back(channel);
	return Save(channels, error);
}

bool MultistreamChannelStore::Remove(const string &channelId, string &error)
{
	auto channels = Load();
	const auto removed = remove_if(channels.begin(), channels.end(),
				       [&](const MultiStreamChannel &item) { return item.id == channelId; });
	if (removed == channels.end()) {
		error.clear();
		return true;
	}
	channels.erase(removed, channels.end());

	string keyError;
	if (!SecureTokenStore::Remove(MANUAL_KEY_STORE, channelId, keyError))
		blog(LOG_WARNING, "Could not remove the stored stream key: %s", keyError.c_str());
	return Save(channels, error);
}

bool MultistreamChannelStore::SetEnabled(const string &channelId, bool enabled, string &error)
{
	auto channels = Load();
	auto item = find_if(channels.begin(), channels.end(),
			    [&](const MultiStreamChannel &channel) { return channel.id == channelId; });
	if (item == channels.end()) {
		error = "The selected multistream channel does not exist.";
		return false;
	}
	item->enabled = enabled;
	return Save(channels, error);
}
