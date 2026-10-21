#include "StudioReplayTemplate.hpp"
#include "AdvancedOutput.hpp"
#include "SimpleOutput.hpp"
#include <widgets/OBSBasic.hpp>
#include <OBSApp.hpp>
#include <algorithm>

namespace {
bool IsPortrait(const MultiStreamChannel &channel)
{
	return channel.videoLayout == MultiStreamVideoLayout::Portrait;
}
bool Matches(const MultiStreamChannel &channel, StudioCanvasTarget canvas)
{
	return channel.enabled && IsPortrait(channel) == (canvas == StudioCanvasTarget::Portrait);
}
std::vector<obs_encoder_t *> Audio(obs_output_t *output)
{
	std::vector<obs_encoder_t *> audio;
	for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
		if (auto *encoder = obs_output_get_audio_encoder(output, i)) {
			audio.push_back(encoder);
		}
	}
	return audio;
}
bool Custom(config_t *config, StudioCanvasTarget canvas)
{
	return QString::fromUtf8(config_get_string(config, "StudioCanvas",
						   canvas == StudioCanvasTarget::Main
							   ? "MainReplayPresetSource"
							   : "PortraitReplayPresetSource")) == "custom";
}
bool AdvancedVod(OBSBasic *main)
{
	auto *config = main->Config();
	if (!config_get_bool(config, "AdvOut", "VodTrackEnabled") ||
	    config_get_int(config, "AdvOut", "TrackIndex") == config_get_int(config, "AdvOut", "VodTrackIndex")) {
		return false;
	}
	auto *service = main->GetService();
	if (!service) {
		return false;
	}
	if (QString::fromUtf8(obs_service_get_id(service)) == "rtmp_custom") {
		return config_get_bool(App()->GetUserConfig(), "General", "EnableCustomServerVodTrack");
	}
	OBSDataAutoRelease settings = obs_service_get_settings(service);
	return ServiceSupportsVodTrack(obs_data_get_string(settings, "service"));
}
} // namespace

bool ResolveStudioReplayTemplate(BasicOutputHandler *handler, StudioCanvasTarget canvas, StudioOutputPreset &result,
				 QString &error)
{
	error.clear();
	if (!handler || !handler->main) {
		error = QStringLiteral("Output handler is unavailable.");
		return false;
	}
	auto *config = handler->main->Config();
	auto *manager = handler->multiStreamManager.get();
	const auto channels = manager ? manager->ConfiguredChannels() : std::vector<MultiStreamChannel>{};
	const auto primaryId = manager ? manager->PrimaryChannelId() : std::string{};
	const auto primary = std::find_if(channels.begin(), channels.end(),
					  [&](const auto &channel) { return channel.id == primaryId; });
	const bool primaryPortrait = primary != channels.end() && IsPortrait(*primary);
	const bool wantPortrait = canvas == StudioCanvasTarget::Portrait;
	const bool custom = Custom(config, canvas);
	OBSOutputAutoRelease primaryOutput = handler->StreamingOutput();
	// An output reference keeps its encoder ownership stable during this UI-thread snapshot.
	if (!custom && primaryOutput && obs_output_active(primaryOutput) && primaryPortrait == wantPortrait) {
		return ResolveStudioReplayPreset(handler->main, canvas, obs_output_get_video_encoder(primaryOutput),
						 Audio(primaryOutput), true, result, error);
	}

	auto outputs = manager ? manager->ChannelOutputs() : std::vector<MultiStreamManager::ChannelOutput>{};
	obs_output_t *selected = custom && primaryOutput && obs_output_active(primaryOutput) &&
						 primaryPortrait == wantPortrait
					 ? primaryOutput.Get()
					 : nullptr;
	StudioOutputPreset selectedPlan;
	for (const auto &entry : outputs) {
		const auto channel = std::find_if(channels.begin(), channels.end(), [&](const auto &c) {
			return c.id == entry.id && Matches(c, canvas);
		});
		if (channel == channels.end() || !entry.output || !obs_output_active(entry.output)) {
			continue;
		}
		StudioOutputPreset candidate;
		if (!ResolveStudioReplayPreset(handler->main, canvas, obs_output_get_video_encoder(entry.output),
					       Audio(entry.output), true, candidate, error)) {
			return false;
		}
		if (selected && !custom && candidate.Summary() != selectedPlan.Summary()) {
			error = QStringLiteral(
				"Several live destinations on this canvas have different audio or video presets. Choose a custom clip preset or make one of them primary.");
			return false;
		}
		if (!selected) {
			selected = entry.output;
			selectedPlan = std::move(candidate);
		}
	}
	if (selected && !custom) {
		result = std::move(selectedPlan);
		return true;
	}

	obs_encoder_t *video = nullptr;
	obs_encoder_t *defaultAudio = nullptr;
	obs_encoder_t *archive = nullptr;
	std::vector<obs_encoder_t *> byMix(MAX_AUDIO_MIXES, nullptr);
	bool includeVod = false;
	if (auto *simple = dynamic_cast<SimpleOutput *>(handler)) {
		video = simple->videoStreaming;
		defaultAudio = simple->audioStreaming;
		archive = simple->audioArchive;
		includeVod = handler->main->GetService() && simple->IsVodTrackEnabled(handler->main->GetService());
		for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
			byMix[i] = simple->streamAudioTrack[i];
		}
	} else if (auto *advanced = dynamic_cast<AdvancedOutput *>(handler)) {
		video = advanced->videoStreaming;
		defaultAudio = advanced->streamAudioEnc;
		archive = advanced->streamArchiveEnc;
		includeVod = AdvancedVod(handler->main);
		for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
			byMix[i] = advanced->streamTrack[i];
		}
	} else {
		error = QStringLiteral("This output mode has no independent clip preset adapter.");
		return false;
	}
	// Configured multistream destinations use the indexed streaming encoders.
	// Canonical default/VOD encoders are selected separately below when there
	// is no configured destination; never replace indexed mix 0 or 1 here.
	const MultiStreamChannel *configured = nullptr;
	if (primary != channels.end() && Matches(*primary, canvas)) {
		configured = &*primary;
	}
	for (const auto &channel : channels) {
		if (!Matches(channel, canvas) || (primary != channels.end() && configured == &*primary)) {
			continue;
		}
		if (configured && !custom &&
		    (configured->audioMixIndex != channel.audioMixIndex ||
		     configured->vodTrackEnabled != channel.vodTrackEnabled ||
		     configured->vodTrackIndex != channel.vodTrackIndex)) {
			error = QStringLiteral(
				"Configured destinations on this canvas use different mixes. Select a primary destination or a custom clip audio mix.");
			return false;
		}
		if (!configured) {
			configured = &channel;
		}
	}
	std::vector<obs_encoder_t *> audio;
	size_t customMix = 0;
	if (custom) {
		const auto mix = config_get_int(config, "StudioCanvas",
						canvas == StudioCanvasTarget::Main ? "MainReplayAudioMix"
										   : "PortraitReplayAudioMix");
		if (mix < 0 || mix >= MAX_AUDIO_MIXES || (!byMix[size_t(mix)] && !defaultAudio)) {
			error = QStringLiteral("The custom clip audio mix is unavailable in this output mode.");
			return false;
		}
		customMix = size_t(mix);
		// Codec/settings may come from the stream template when a Simple
		// recording preset did not instantiate this mix. The new replay audio
		// encoder still receives the explicitly requested mixer index.
		audio.push_back(byMix[customMix] ? byMix[customMix] : defaultAudio);
	} else if (configured) {
		if (configured->audioMixIndex >= byMix.size() || !byMix[configured->audioMixIndex]) {
			error = QStringLiteral(
				"The configured stream audio mix is unavailable; no substitute mix was selected.");
			return false;
		}
		audio.push_back(byMix[configured->audioMixIndex]);
		if (configured->vodTrackEnabled && GetStreamPlatformInfo(configured->platform).supportsVodTrack &&
		    configured->vodTrackIndex != configured->audioMixIndex) {
			if (configured->vodTrackIndex >= byMix.size() || !byMix[configured->vodTrackIndex]) {
				error = QStringLiteral("The configured VOD mix is unavailable.");
				return false;
			}
			audio.push_back(byMix[configured->vodTrackIndex]);
		}
	} else {
		audio.push_back(defaultAudio);
		if (includeVod) {
			audio.push_back(archive);
		}
	}
	// A custom preset chooses its audio mix explicitly even while a different
	// destination is live. Its video template still follows the live canvas.
	if (custom && selected) {
		video = obs_output_get_video_encoder(selected);
	}
	if (!ResolveStudioReplayPreset(handler->main, canvas, video, audio, custom && selected, result, error)) {
		return false;
	}
	if (custom && !result.audio.empty()) {
		result.audio.front().mixerIndex = customMix;
	}
	return true;
}
