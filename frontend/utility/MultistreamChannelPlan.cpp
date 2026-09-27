/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamChannelPlan.hpp"

#include <algorithm>

using namespace std;

namespace MultistreamChannelPlan {
MultiStreamChannel FirstReady(const vector<MultiStreamChannel> &channels)
{
	auto ready = find_if(channels.begin(), channels.end(), [](const MultiStreamChannel &channel) {
		return channel.videoLayout == MultiStreamVideoLayout::Main && channel.enabled &&
		       !channel.server.empty() && !channel.streamKey.empty();
	});
	if (ready == channels.end()) {
		ready = find_if(channels.begin(), channels.end(), [](const MultiStreamChannel &channel) {
			return channel.enabled && !channel.server.empty() && !channel.streamKey.empty();
		});
	}
	return ready != channels.end() ? *ready : MultiStreamChannel{};
}

void NormalizePortraitFit(vector<MultiStreamChannel> &channels)
{
	const auto portrait = find_if(channels.cbegin(), channels.cend(), [](const MultiStreamChannel &channel) {
		return channel.videoLayout == MultiStreamVideoLayout::Portrait;
	});
	if (portrait == channels.cend())
		return;
	for (auto &channel : channels) {
		if (channel.videoLayout == MultiStreamVideoLayout::Portrait)
			channel.portraitFit = portrait->portraitFit;
	}
}

bool RequiresH264(StreamPlatform platform)
{
	return platform != StreamPlatform::YouTube && platform != StreamPlatform::CustomRtmp;
}
} // namespace MultistreamChannelPlan
