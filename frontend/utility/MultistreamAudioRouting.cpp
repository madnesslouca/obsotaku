#include "MultistreamAudioRouting.hpp"
#include "MultiStreamManager.hpp"

#include <algorithm>

bool PrepareMultistreamPrimaryAudio(const MultiStreamManager &manager, obs_output_t *output,
				   const std::vector<obs_encoder_t *> &tracks, std::string &error)
{
	const auto primaryId = manager.PrimaryChannelId();
	if (primaryId.empty()) {
		error.clear();
		return true;
	}
	const auto channels = manager.ConfiguredChannels();
	const auto primary = std::find_if(channels.begin(), channels.end(), [&](const auto &channel) {
		return channel.id == primaryId && channel.enabled;
	});
	if (!output || obs_output_active(output) || primary == channels.end()) {
		error = "The multistream primary output is unavailable or already active.";
		return false;
	}
	obs_encoder_t *audio = primary->audioMixIndex < tracks.size() ? tracks[primary->audioMixIndex] : nullptr;
	if (!audio) {
		auto *current = obs_output_get_audio_encoder(output, 0);
		if (current && obs_encoder_get_mixer_index(current) == primary->audioMixIndex)
			audio = current;
	}
	if (!audio || obs_encoder_get_mixer_index(audio) != primary->audioMixIndex) {
		error = "Audio track " + std::to_string(primary->audioMixIndex + 1) + " is unavailable for primary channel " +
			primary->displayName + ". No substitute track was sent.";
		return false;
	}
	obs_encoder_t *vod = nullptr;
	if (primary->vodTrackEnabled && GetStreamPlatformInfo(primary->platform).supportsVodTrack &&
	    primary->vodTrackIndex != primary->audioMixIndex) {
		vod = primary->vodTrackIndex < tracks.size() ? tracks[primary->vodTrackIndex] : nullptr;
		if (!vod || obs_encoder_get_mixer_index(vod) != primary->vodTrackIndex) {
			error = "The selected VOD audio track is unavailable for primary channel " + primary->displayName + ".";
			return false;
		}
	}
	obs_output_set_audio_encoder(output, audio, 0);
	obs_output_set_audio_encoder(output, vod, 1);
	error.clear();
	return true;
}
