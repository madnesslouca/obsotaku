/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamPreflight.hpp"
#include "MultistreamChannelPlan.hpp"

#include <widgets/OBSBasic.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <obs.hpp>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <QString>

#include <algorithm>
#include <filesystem>

using namespace std;

const char *get_simple_output_encoder(const char *encoder);

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

/* Advanced mode keeps the stream bitrate in the encoder's own settings file
 * rather than in the profile ini. Every encoder OBS ships names the field
 * "bitrate", so reading it there covers them all; an encoder that does not is
 * simply reported as unknown, exactly as before. */
uint32_t AdvancedVideoBitrate()
{
	const OBSBasic *main = OBSBasic::Get();
	if (!main)
		return 0;

	const std::filesystem::path path =
		main->GetCurrentProfile().path / std::filesystem::u8path("streamEncoder.json");
	if (path.empty())
		return 0;

	BPtr<char> json = os_quick_read_utf8_file(path.u8string().c_str());
	if (!json)
		return 0;

	OBSDataAutoRelease settings = obs_data_create_from_json(json);
	return settings ? static_cast<uint32_t>(obs_data_get_int(settings, "bitrate")) : 0;
}

uint32_t AdvancedAudioBitrate(config_t *config)
{
	/* The stream track is 1-based in the interface and the key is per track. */
	const int64_t track = config_get_int(config, "AdvOut", "TrackIndex");
	const string key = "Track" + to_string(track < 1 ? 1 : track) + "Bitrate";
	return static_cast<uint32_t>(config_get_uint(config, "AdvOut", key.c_str()));
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

	const char *mode = config_get_string(config, "Output", "Mode");
	if (mode && astrcmpi(mode, "Advanced") == 0) {
		settings.videoBitrateKbps = AdvancedVideoBitrate();
		settings.audioBitrateKbps = AdvancedAudioBitrate(config);
		const char *encoderId = config_get_string(config, "AdvOut", "Encoder");
		const char *codec = encoderId ? obs_get_encoder_codec(encoderId) : nullptr;
		settings.videoCodec = codec ? codec : "";
	} else {
		settings.videoBitrateKbps = SimpleVideoBitrate(config);
		settings.audioBitrateKbps = SimpleAudioBitrate(config);
		const char *simpleEncoder = config_get_string(config, "SimpleOutput", "StreamEncoder");
		const char *encoderId = get_simple_output_encoder(simpleEncoder ? simpleEncoder : "");
		const char *codec = encoderId ? obs_get_encoder_codec(encoderId) : nullptr;
		settings.videoCodec = codec ? codec : "";
	}
	return settings;
}

vector<PreflightFinding> MultistreamPreflight::Check(const vector<MultiStreamChannel> &channels,
						     const OutputVideoSettings &settings)
{
	vector<PreflightFinding> findings;
	uint64_t estimatedUploadKbps = 0;
	bool countedUpload = false;

	for (const auto &channel : channels) {
		if (!channel.enabled)
			continue;
		const QString name = QString::fromStdString(channel.displayName);
		if (channel.server.empty() || channel.streamKey.empty()) {
			findings.push_back({channel.id, channel.displayName, PreflightSeverity::Warning,
					    QT_TO_UTF8(QTStr("Multistream.Preflight.MissingCredentials").arg(name))});
			continue;
		}

		const auto &limits = GetStreamPlatformInfo(channel.platform).limits;
		OutputVideoSettings channelSettings = settings;
		if (channel.videoLayout == MultiStreamVideoLayout::Portrait) {
			channelSettings.width = 1080;
			channelSettings.height = 1920;
			channelSettings.framerate = min(channelSettings.framerate, 30u);
			channelSettings.videoBitrateKbps = channelSettings.videoBitrateKbps == 0
								   ? 6000
								   : min(channelSettings.videoBitrateKbps, 6000u);
			channelSettings.videoCodec = "h264";
		}
		const bool outputIsPortrait = channelSettings.height > channelSettings.width;
		const QString platform = PlatformName(channel.platform);
		estimatedUploadKbps += channelSettings.videoBitrateKbps + channelSettings.audioBitrateKbps;
		countedUpload = countedUpload || channelSettings.videoBitrateKbps > 0;

		auto add = [&](PreflightSeverity severity, const QString &message) {
			findings.push_back({channel.id, channel.displayName, severity, QT_TO_UTF8(message)});
		};

		if (channelSettings.width > 0 && channelSettings.height > 0 &&
		    limits.preferredOrientation == StreamOrientation::Portrait && !outputIsPortrait) {
			add(PreflightSeverity::Warning, QTStr("Multistream.Preflight.NeedsPortrait")
								.arg(name, platform)
								.arg(channelSettings.width)
								.arg(channelSettings.height));
		} else if (channelSettings.width > 0 && channelSettings.height > 0 &&
			   limits.preferredOrientation == StreamOrientation::Landscape && outputIsPortrait) {
			add(PreflightSeverity::Warning, QTStr("Multistream.Preflight.NeedsLandscape")
								.arg(name, platform)
								.arg(channelSettings.width)
								.arg(channelSettings.height));
		}

		/* Compare the long and short edges instead of width/height so a
		 * vertical canvas is not reported as exceeding a horizontal limit. */
		const uint32_t longEdge = max(channelSettings.width, channelSettings.height);
		const uint32_t shortEdge = min(channelSettings.width, channelSettings.height);
		const uint32_t limitLong = max(limits.maxWidth, limits.maxHeight);
		const uint32_t limitShort = min(limits.maxWidth, limits.maxHeight);
		if (limitLong > 0 && (longEdge > limitLong || shortEdge > limitShort)) {
			add(PreflightSeverity::Advisory, QTStr("Multistream.Preflight.ResolutionTooHigh")
								 .arg(name, platform)
								 .arg(channelSettings.width)
								 .arg(channelSettings.height)
								 .arg(limits.maxWidth)
								 .arg(limits.maxHeight));
		}

		if (limits.maxFramerate > 0 && channelSettings.framerate > limits.maxFramerate) {
			add(PreflightSeverity::Advisory, QTStr("Multistream.Preflight.FramerateTooHigh")
								 .arg(name, platform)
								 .arg(channelSettings.framerate)
								 .arg(limits.maxFramerate));
		}

		if (limits.maxVideoBitrateKbps > 0 &&
		    channelSettings.videoBitrateKbps > limits.maxVideoBitrateKbps) {
			add(PreflightSeverity::Warning, QTStr("Multistream.Preflight.BitrateTooHigh")
								.arg(name, platform)
								.arg(channelSettings.videoBitrateKbps)
								.arg(limits.maxVideoBitrateKbps));
		}

		if (limits.maxAudioBitrateKbps > 0 && settings.audioBitrateKbps > limits.maxAudioBitrateKbps) {
			add(PreflightSeverity::Advisory, QTStr("Multistream.Preflight.AudioBitrateTooHigh")
								 .arg(name, platform)
								 .arg(settings.audioBitrateKbps)
								 .arg(limits.maxAudioBitrateKbps));
		}

		const bool requiresH264 = MultistreamChannelPlan::RequiresH264(channel.platform);
		if (requiresH264 && !channelSettings.videoCodec.empty() && channelSettings.videoCodec != "h264") {
			add(PreflightSeverity::Warning,
			    QTStr("Multistream.Preflight.CodecUnsupported")
				    .arg(name, platform, QString::fromStdString(channelSettings.videoCodec).toUpper()));
		}
	}

	if (countedUpload) {
		const uint64_t recommendedKbps = (estimatedUploadKbps * 125 + 99) / 100;
		findings.push_back({{}, {}, PreflightSeverity::Advisory,
				    QT_TO_UTF8(QTStr("Multistream.Preflight.EstimatedUpload")
						.arg(estimatedUploadKbps)
						.arg(recommendedKbps))});
	}

	return findings;
}
