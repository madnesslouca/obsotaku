#include "StudioOutputPreset.hpp"
#include <QJsonArray>

namespace {
OBSData Freeze(obs_data_t *data)
{
	if (!data) {
		return {};
	}
	const char *json = obs_data_get_json_with_defaults(data);
	OBSDataAutoRelease frozen = obs_data_create_from_json(json ? json : "{}");
	return frozen.Get();
}
} // namespace

StudioAudioPreset::StudioAudioPreset(const StudioAudioPreset &other)
{
	*this = other;
}
StudioAudioPreset &StudioAudioPreset::operator=(const StudioAudioPreset &other)
{
	if (this != &other) {
		encoderId = other.encoderId;
		settings = Freeze(other.settings);
		mixerIndex = other.mixerIndex;
	}
	return *this;
}
StudioOutputPreset::StudioOutputPreset(const StudioOutputPreset &other)
{
	*this = other;
}
StudioOutputPreset &StudioOutputPreset::operator=(const StudioOutputPreset &other)
{
	if (this != &other) {
		canvas = other.canvas;
		origin = other.origin;
		encoderId = other.encoderId;
		videoSettings = Freeze(other.videoSettings);
		video = other.video;
		width = other.width;
		height = other.height;
		fpsDivisor = other.fpsDivisor;
		preferredFormat = other.preferredFormat;
		scaleType = other.scaleType;
		audio = other.audio;
	}
	return *this;
}
QJsonObject StudioOutputPreset::Summary() const
{
	QJsonArray tracks;
	for (const auto &track : audio) {
		tracks.append(QJsonObject{{"encoder_id", track.encoderId},
					  {"mixer", int(track.mixerIndex)},
					  {"bitrate_kbps", int(obs_data_get_int(track.settings, "bitrate"))}});
	}
	return {{"canvas", canvas == StudioCanvasTarget::Main ? "main" : "portrait"},
		{"origin", origin},
		{"encoder_id", encoderId},
		{"width", int(width)},
		{"height", int(height)},
		{"fps_divisor", int(fpsDivisor)},
		{"preferred_format", int(preferredFormat)},
		{"scale_type", int(scaleType)},
		{"rate_control", QString::fromUtf8(obs_data_get_string(videoSettings, "rate_control"))},
		{"bitrate_kbps", int(obs_data_get_int(videoSettings, "bitrate"))},
		{"audio", tracks}};
}
