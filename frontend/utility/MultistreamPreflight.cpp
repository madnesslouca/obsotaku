/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamPreflight.hpp"

#include <widgets/OBSBasic.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <obs.hpp>
#include <util/config-file.h>

#include <QString>

#include <algorithm>

using namespace std;

namespace {
QString PlatformName(StreamPlatform platform)
{
	const auto name = GetStreamPlatformInfo(platform).displayName;
	return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

uint32_t SimpleVideoBitrate(config_t *config)
{
	return static_cast<uint32_t>(config_get_uint(config, "SimpleOutput", "VBitrate"));
}

uint32_t SimpleAudioBitrate(config_t *config)
{
	return static_cast<uint32_t>(config_get_uint(config, "SimpleOutput", "ABitrate"));
}
} // namespace

OutputVideoSettings MultistreamPreflight::CurrentOutputSettings()
{
	OutputVideoSettings settings;

	/* The running video pipeline is the truth while streaming; the profile is
	 * the truth before it starts. obs_get_video_info covers both. */
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		settings.width = ovi.output_width;
		settings.height = ovi.output_height;
		if (ovi.fps_den > 0)
			settings.framerate = static_cast<uint32_t>((ovi.fps_num + ovi.fps_den - 1) / ovi.fps_den);
	}

	OBSBasic *main = OBSBasic::Get();
	config_t *config = main ? main->Config() : nullptr;
	if (!config)
		return settings;

	if (settings.width == 0 || settings.height == 0) {
		settings.width = static_cast<uint32_t>(config_get_uint(config, "Video", "OutputCX"));
		settings.height = static_cast<uint32_t>(config_get_uint(config, "Video", "OutputCY"));
	}

	/* Advanced mode keeps the bitrate inside the encoder settings, which vary
	 * per encoder, so only simple mode reports one here. */
	const char *mode = config_get_string(config, "Output", "Mode");
	if (!mode || astrcmpi(mode, "Advanced") != 0) {
		settings.videoBitrateKbps = SimpleVideoBitrate(config);
		settings.audioBitrateKbps = SimpleAudioBitrate(config);
	}
	return settings;
}

vector<PreflightFinding> MultistreamPreflight::Check(const vector<MultiStreamChannel> &channels,
						     const OutputVideoSettings &settings)
{
	vector<PreflightFinding> findings;
	if (settings.width == 0 || settings.height == 0)
		return findings;

	const bool outputIsPortrait = settings.height > settings.width;

	for (const auto &channel : channels) {
		if (!channel.enabled)
			continue;

		const auto &limits = GetStreamPlatformInfo(channel.platform).limits;
		const QString platform = PlatformName(channel.platform);
		const QString name = QString::fromStdString(channel.displayName);

		auto add = [&](PreflightSeverity severity, const QString &message) {
			findings.push_back({channel.id, channel.displayName, severity, QT_TO_UTF8(message)});
		};

		if (limits.preferredOrientation == StreamOrientation::Portrait && !outputIsPortrait) {
			add(PreflightSeverity::Warning, QTStr("Multistream.Preflight.NeedsPortrait")
								.arg(name, platform)
								.arg(settings.width)
								.arg(settings.height));
		} else if (limits.preferredOrientation == StreamOrientation::Landscape && outputIsPortrait) {
			add(PreflightSeverity::Warning, QTStr("Multistream.Preflight.NeedsLandscape")
								.arg(name, platform)
								.arg(settings.width)
								.arg(settings.height));
		}

		/* Compare the long and short edges instead of width/height so a
		 * vertical canvas is not reported as exceeding a horizontal limit. */
		const uint32_t longEdge = max(settings.width, settings.height);
		const uint32_t shortEdge = min(settings.width, settings.height);
		const uint32_t limitLong = max(limits.maxWidth, limits.maxHeight);
		const uint32_t limitShort = min(limits.maxWidth, limits.maxHeight);
		if (limitLong > 0 && (longEdge > limitLong || shortEdge > limitShort)) {
			add(PreflightSeverity::Advisory, QTStr("Multistream.Preflight.ResolutionTooHigh")
								 .arg(name, platform)
								 .arg(settings.width)
								 .arg(settings.height)
								 .arg(limits.maxWidth)
								 .arg(limits.maxHeight));
		}

		if (limits.maxFramerate > 0 && settings.framerate > limits.maxFramerate) {
			add(PreflightSeverity::Advisory, QTStr("Multistream.Preflight.FramerateTooHigh")
								 .arg(name, platform)
								 .arg(settings.framerate)
								 .arg(limits.maxFramerate));
		}

		if (limits.maxVideoBitrateKbps > 0 && settings.videoBitrateKbps > limits.maxVideoBitrateKbps) {
			add(PreflightSeverity::Warning, QTStr("Multistream.Preflight.BitrateTooHigh")
								.arg(name, platform)
								.arg(settings.videoBitrateKbps)
								.arg(limits.maxVideoBitrateKbps));
		}

		if (limits.maxAudioBitrateKbps > 0 && settings.audioBitrateKbps > limits.maxAudioBitrateKbps) {
			add(PreflightSeverity::Advisory, QTStr("Multistream.Preflight.AudioBitrateTooHigh")
								 .arg(name, platform)
								 .arg(settings.audioBitrateKbps)
								 .arg(limits.maxAudioBitrateKbps));
		}
	}

	return findings;
}
