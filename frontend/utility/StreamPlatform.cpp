/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "StreamPlatform.hpp"

#include <algorithm>

using namespace std;

const vector<StreamPlatformInfo> &SupportedStreamPlatforms()
{
	static const vector<StreamPlatformInfo> platforms = {
		{
			StreamPlatform::YouTube,
			"youtube",
			"YouTube",
			StreamOAuthFlow::AuthorizationCodePkce,
			StreamIngestMode::ResolvedByApi,
			"https://accounts.google.com/o/oauth2/v2/auth",
			{},
			"https://oauth2.googleapis.com/token",
			"https://www.googleapis.com/",
			"youtube/v3/liveStreams?part=cdn&mine=true",
			{},
			{},
			"#ff0000",
			/* youtube.com/help — 1440p60 tops the recommended ingest table. */
			{2560, 1440, 60, 51000, 128, StreamOrientation::Any},
			{"https://www.googleapis.com/auth/youtube"},
			true,
			true,
			false,
			true,
			true,
			true,
			false,
		},
		{
			StreamPlatform::Twitch,
			"twitch",
			"Twitch",
			StreamOAuthFlow::DeviceCode,
			StreamIngestMode::ResolvedByApi,
			"https://id.twitch.tv/oauth2/authorize",
			"https://id.twitch.tv/oauth2/device",
			"https://id.twitch.tv/oauth2/token",
			"https://api.twitch.tv/",
			"helix/streams/key",
			{},
			{},
			"#9146ff",
			/* Twitch ingest guide: 1080p60, 6000 kbps for non-partners. */
			{1920, 1080, 60, 6000, 160, StreamOrientation::Landscape},
			/* chat:read / chat:edit let the unified dock join IRC as the
			 * streamer and reply from OBS. Existing connections must
			 * reconnect once to pick up the new scopes. */
			{"channel:read:stream_key", "channel:manage:broadcast", "chat:read", "chat:edit"},
			false,
			true,
			false,
			false,
			true,
			true,
			/* Twitch is the only platform that accepts a separate VOD track. */
			true,
		},
		{
			StreamPlatform::Kick,
			"kick",
			"Kick",
			StreamOAuthFlow::AuthorizationCodePkce,
			StreamIngestMode::ResolvedByApi,
			"https://id.kick.com/oauth/authorize",
			{},
			"https://id.kick.com/oauth/token",
			"https://api.kick.com/",
			"public/v1/channels",
			{},
			{},
			"#53fc18",
			/* Kick ingest guide: 1080p60 at 8000 kbps. */
			{1920, 1080, 60, 8000, 160, StreamOrientation::Landscape},
			{"channel:read", "streamkey:read", "channel:write", "user:read"},
			true,
			true,
			true,
			false,
			true,
			true,
			false,
		},
		{
			/* Meta only hands out ingest credentials through Live Producer
			 * unless the application passes App Review for publish_video. */
			StreamPlatform::Facebook,
			"facebook",
			"Facebook Live",
			StreamOAuthFlow::None,
			StreamIngestMode::ManualStreamKey,
			{},
			{},
			{},
			{},
			{},
			"rtmps://live-api-s.facebook.com:443/rtmp/",
			"https://www.facebook.com/live/producer",
			"#0866ff",
			/* Facebook Live specs: 1080p60, 4000 kbps video, 128 kbps audio. */
			{1920, 1080, 60, 4000, 128, StreamOrientation::Any},
			{},
			false,
			false,
			false,
			false,
			false,
			false,
			false,
		},
		{
			/* TikTok LIVE ingest is only exposed through TikTok Live Studio
			 * for accounts allowed to stream; there is no public API.
			 *
			 * No default server on purpose: TikTok hands out the server and
			 * the key together, per session, and the host differs by region.
			 * Pre-filling one would look like only the key was missing, and
			 * point the user at a datacenter that is not theirs. */
			StreamPlatform::TikTok,
			"tiktok",
			"TikTok",
			StreamOAuthFlow::None,
			StreamIngestMode::ManualStreamKey,
			{},
			{},
			{},
			{},
			{},
			{},
			"https://livecenter.tiktok.com/",
			"#ff0050",
			/* TikTok LIVE is a vertical surface: 1080x1920 at 30 fps. */
			{1080, 1920, 30, 6000, 128, StreamOrientation::Portrait},
			{},
			false,
			false,
			false,
			false,
			false,
			false,
			false,
		},
		{
			StreamPlatform::X,
			"x",
			"X",
			StreamOAuthFlow::None,
			StreamIngestMode::ManualStreamKey,
			{},
			{},
			{},
			{},
			{},
			"rtmps://va.pscp.tv:443/x/",
			"https://studio.x.com/producer/sources",
			/* The X mark is black on white; a neutral gray is the only
			 * choice that stays visible in both light and dark themes. */
			"#71767b",
			/* X Producer caps ingest at 1080p30 / 12000 kbps. */
			{1920, 1080, 30, 12000, 128, StreamOrientation::Any},
			{},
			false,
			false,
			false,
			false,
			false,
			false,
			false,
		},
		{
			StreamPlatform::Trovo,
			"trovo",
			"Trovo",
			StreamOAuthFlow::None,
			StreamIngestMode::ManualStreamKey,
			{},
			{},
			{},
			{},
			{},
			"rtmp://livepush.trovo.live/live/",
			"https://studio.trovo.live/mychannel/stream",
			"#1ec160",
			/* Trovo streamer guide: 1080p60 at 8000 kbps. */
			{1920, 1080, 60, 8000, 128, StreamOrientation::Landscape},
			{},
			false,
			false,
			false,
			false,
			false,
			false,
			false,
		},
		{
			StreamPlatform::CustomRtmp,
			"custom-rtmp",
			"Custom RTMP",
			StreamOAuthFlow::None,
			StreamIngestMode::ManualStreamKey,
			{},
			{},
			{},
			{},
			{},
			{},
			{},
			"#8e8e93",
			/* An arbitrary RTMP server publishes no limits we could check. */
			{},
			{},
			false,
			false,
			false,
			false,
			false,
			false,
			false,
		},
	};

	return platforms;
}

const StreamPlatformInfo &GetStreamPlatformInfo(StreamPlatform platform)
{
	const auto &platforms = SupportedStreamPlatforms();
	const auto item = find_if(platforms.cbegin(), platforms.cend(),
				  [platform](const StreamPlatformInfo &info) { return info.platform == platform; });
	return item != platforms.cend() ? *item : platforms.back();
}

optional<StreamPlatform> StreamPlatformFromId(string_view id)
{
	const auto &platforms = SupportedStreamPlatforms();
	const auto item = find_if(platforms.cbegin(), platforms.cend(),
				  [id](const StreamPlatformInfo &info) { return info.id == id; });
	return item != platforms.cend() ? optional{item->platform} : nullopt;
}

const vector<StreamPlatform> &SelectableStreamPlatforms()
{
	static const vector<StreamPlatform> platforms = {
		StreamPlatform::Twitch,  StreamPlatform::YouTube, StreamPlatform::Kick,  StreamPlatform::Facebook,
		StreamPlatform::TikTok,  StreamPlatform::X,       StreamPlatform::Trovo, StreamPlatform::CustomRtmp,
	};
	return platforms;
}

string StreamPlatformConfigSection(StreamPlatform platform)
{
	return "MultistreamAccount." + string(GetStreamPlatformInfo(platform).id);
}
