#pragma once

#include <obs.hpp>
#include <util/config-file.h>
#include <QJsonObject>
#include <QString>
#include <vector>

class OBSBasic;
enum class StudioCanvasTarget { Main, Portrait };

/* Each copy freezes nested settings. No replay reuses a stream/record encoder.
 * The caller retains the OBSCanvas that owns the borrowed video_t. */
struct StudioAudioPreset {
	QString encoderId;
	OBSData settings;
	size_t mixerIndex = 0;
	StudioAudioPreset() = default;
	StudioAudioPreset(const StudioAudioPreset &other);
	StudioAudioPreset &operator=(const StudioAudioPreset &other);
	StudioAudioPreset(StudioAudioPreset &&) = default;
	StudioAudioPreset &operator=(StudioAudioPreset &&) = default;
};
struct StudioOutputPreset {
	StudioCanvasTarget canvas = StudioCanvasTarget::Main;
	QString origin;
	QString encoderId;
	OBSData videoSettings;
	video_t *video = nullptr;
	uint32_t width = 0, height = 0, fpsDivisor = 1;
	video_format preferredFormat = VIDEO_FORMAT_NONE;
	obs_scale_type scaleType = OBS_SCALE_BICUBIC;
	std::vector<StudioAudioPreset> audio;
	StudioOutputPreset() = default;
	StudioOutputPreset(const StudioOutputPreset &other);
	StudioOutputPreset &operator=(const StudioOutputPreset &other);
	StudioOutputPreset(StudioOutputPreset &&) = default;
	StudioOutputPreset &operator=(StudioOutputPreset &&) = default;
	QJsonObject Summary() const;
};

/* Effective encoders come from the output transmitting this canvas. Offline
 * templates come from Simple/Advanced streaming encoders; profile and service
 * policy is applied to private data without Update()/SetupStreaming()/network.
 * For offline Portrait the caller replaces result.video with its retained
 * Portrait canvas video. */
bool ResolveStudioReplayPreset(OBSBasic *main, StudioCanvasTarget canvas, obs_encoder_t *streamVideo,
			       const std::vector<obs_encoder_t *> &streamAudio, bool streamIsEffective,
			       StudioOutputPreset &result, QString &error);

/* Downscale only, before encoder start. Explicit Studio preference overrides
 * native Advanced Rescale; absent means retain native behavior. */
bool ApplyStudioMainStreamScale(config_t *config, obs_encoder_t *encoder, QString *error = nullptr);
