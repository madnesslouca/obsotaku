/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class StreamPlatform {
	YouTube,
	Twitch,
	Kick,
	Facebook,
	TikTok,
	X,
	Trovo,
	CustomRtmp,
};

enum class StreamOAuthFlow {
	None,
	AuthorizationCodePkce,
	DeviceCode,
};

enum class StreamOrientation {
	Any,
	Landscape,
	Portrait,
};

/* Published ingest limits, used to warn before going live. Zero means the
 * platform does not publish a limit for that dimension. */
struct StreamPlatformLimits {
	uint32_t maxWidth = 0;
	uint32_t maxHeight = 0;
	uint32_t maxFramerate = 0;
	uint32_t maxVideoBitrateKbps = 0;
	uint32_t maxAudioBitrateKbps = 0;
	StreamOrientation preferredOrientation = StreamOrientation::Any;
};

enum class StreamIngestMode {
	/* The platform exposes an API that returns the ingest server and stream
	 * key for the signed-in account. */
	ResolvedByApi,
	/* No public API: the user pastes the stream key their platform dashboard
	 * shows. The server is pre-filled from defaultIngestServer. */
	ManualStreamKey,
};

struct StreamPlatformInfo {
	StreamPlatform platform;
	std::string_view id;
	std::string_view displayName;
	StreamOAuthFlow oauthFlow;
	StreamIngestMode ingestMode;
	std::string_view authorizationEndpoint;
	std::string_view deviceAuthorizationEndpoint;
	std::string_view tokenEndpoint;
	std::string_view apiBaseUrl;
	std::string_view ingestCredentialEndpoint;
	/* Ingest server used by the manual flow, and the page where the user
	 * finds their stream key. Empty for API-resolved platforms. */
	std::string_view defaultIngestServer;
	std::string_view streamKeyHelpUrl;
	/* Brand color, used by the channel cards and the add-channel grid. */
	std::string_view brandColor;
	StreamPlatformLimits limits;
	std::vector<std::string_view> scopes;
	bool apiProvidesIngestServer;
	bool apiProvidesStreamKey;
	bool requiresBackendTokenExchange;
	bool supportsBroadcastCreation;
	bool supportsMetadataUpdates;
	/* Whether the unified chat dock can read this platform's chat. */
	bool supportsChat;
	/* Whether the platform accepts a second audio track for the recorded
	 * VOD, so the live mix and the archive can differ (Twitch only today). */
	bool supportsVodTrack;
};

const std::vector<StreamPlatformInfo> &SupportedStreamPlatforms();
const StreamPlatformInfo &GetStreamPlatformInfo(StreamPlatform platform);
std::optional<StreamPlatform> StreamPlatformFromId(std::string_view id);

/* Platforms offered in the add-channel grid, in display order. */
const std::vector<StreamPlatform> &SelectableStreamPlatforms();

/* Single source of truth for the user-configuration section of a connected
 * account. Every component that reads or writes account metadata must use it,
 * otherwise the account state silently splits across sections. */
std::string StreamPlatformConfigSection(StreamPlatform platform);
