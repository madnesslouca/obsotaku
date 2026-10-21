#include "StudioOutputPreset.hpp"
#include <widgets/OBSBasic.hpp>
#include <utility/audio-encoders.hpp>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cstring>

namespace {
OBSData Freeze(obs_data_t *data)
{
	if (!data) {
		return {};
	}
	// Encoder defaults are separate from user values. Apply them before the
	// JSON roundtrip so a snapshot does not silently lose an effective default.
	const char *json = obs_data_get_json_with_defaults(data);
	OBSDataAutoRelease frozen = obs_data_create_from_json(json ? json : "{}");
	return frozen.Get();
}
OBSData EncoderData(obs_encoder_t *encoder)
{
	OBSDataAutoRelease defaults = obs_encoder_get_defaults(encoder);
	OBSDataAutoRelease current = obs_encoder_get_settings(encoder);
	obs_data_apply(defaults, current);
	return Freeze(defaults);
}
bool Size(const QString &text, uint32_t &width, uint32_t &height)
{
	const auto match = QRegularExpression("^([0-9]{2,5})x([0-9]{2,5})$").match(text.trimmed());
	if (!match.hasMatch()) {
		return false;
	}
	width = match.captured(1).toUInt();
	height = match.captured(2).toUInt();
	return width >= 16 && height >= 16 && width <= 16384 && height <= 16384 && !(width & 1) && !(height & 1);
}
QString Resolution(config_t *config)
{
	return QString::fromUtf8(config_get_string(config, "StudioCanvas", "MainStreamResolution"));
}
QString PortraitH264(const QString &mainId)
{
	if (QString::fromUtf8(obs_get_encoder_codec(mainId.toUtf8().constData())) == "h264") {
		return mainId;
	}
	QStringList candidates;
	if (mainId.contains("nvenc")) {
		candidates << "obs_nvenc_h264_tex" << "ffmpeg_nvenc";
	} else if (mainId.contains("qsv")) {
		candidates << "obs_qsv11_v2" << "obs_qsv11";
	} else if (mainId.contains("amf")) {
		candidates << "h264_texture_amf";
	} else if (mainId.contains("videotoolbox") || mainId.contains("apple")) {
		candidates << "com.apple.videotoolbox.videoencoder.ave.avc";
	}
	candidates << "obs_x264";
	for (const auto &id : candidates) {
		if (obs_get_encoder_codec(id.toUtf8().constData())) {
			return id;
		}
	}
	return {};
}
void OfflineVideo(OBSBasic *main, obs_encoder_t *encoder, OBSData &settings)
{
	auto *config = main->Config();
	const bool simple = QString::fromUtf8(config_get_string(config, "Output", "Mode")) != "Advanced";
	if (simple) {
		const QString shortId = QString::fromUtf8(config_get_string(config, "SimpleOutput", "StreamEncoder"));
		const char *presetKey = shortId.contains("qsv")       ? "QSVPreset"
					: shortId.contains("amd_av1") ? "AMDAV1Preset"
					: shortId.contains("amd")     ? "AMDPreset"
					: shortId.contains("nvenc")   ? "NVENCPreset2"
								      : "Preset";
		const char *preset = config_get_string(config, "SimpleOutput", presetKey);
		const QString realId = QString::fromUtf8(obs_encoder_get_id(encoder));
		obs_data_set_string(settings,
				    realId.startsWith("ffmpeg_") && !strcmp(presetKey, "NVENCPreset2") ? "preset2"
												       : "preset",
				    preset);
		obs_data_set_string(settings, "rate_control", "CBR");
		obs_data_set_int(settings, "bitrate", config_get_int(config, "SimpleOutput", "VBitrate"));
		if (config_get_bool(config, "SimpleOutput", "UseAdvanced")) {
			obs_data_set_string(settings, "x264opts",
					    config_get_string(config, "SimpleOutput", "x264Settings"));
		}
	} else {
		const auto path = main->GetCurrentProfile().path / "streamEncoder.json";
		QFile file(QString::fromStdString(path.u8string()));
		if (file.open(QIODevice::ReadOnly)) {
			OBSDataAutoRelease user = obs_data_create_from_json(file.readAll().constData());
			if (user) {
				obs_data_apply(settings, user);
			}
		}
	}
	const auto bitrate = obs_data_get_int(settings, "bitrate");
	const auto keyint = obs_data_get_int(settings, "keyint_sec");
	if ((simple || config_get_bool(config, "AdvOut", "ApplyServiceSettings")) && main->GetService()) {
		obs_service_apply_encoder_settings(main->GetService(), settings, nullptr);
		if (config_get_bool(config, "Stream1", "IgnoreRecommended")) {
			obs_data_set_int(settings, "bitrate", bitrate);
		}
		if (!simple && keyint && keyint < obs_data_get_int(settings, "keyint_sec")) {
			obs_data_set_int(settings, "keyint_sec", keyint);
		}
	}
	if (!simple && config_get_bool(config, "Output", "DynamicBitrate") &&
	    QString::fromUtf8(obs_encoder_get_id(encoder)).contains("nvenc")) {
		obs_data_set_bool(settings, "lookahead", false);
	}
}
} // namespace

bool ApplyStudioMainStreamScale(config_t *config, obs_encoder_t *encoder, QString *error)
{
	const QString requested = Resolution(config);
	if (requested.isEmpty()) {
		return true;
	}
	uint32_t width = 0, height = 0;
	video_t *video = encoder ? obs_encoder_parent_video(encoder) : nullptr;
	if (!video) {
		video = obs_get_video();
	}
	if (!encoder || !video || !Size(requested, width, height) || width > video_output_get_width(video) ||
	    height > video_output_get_height(video)) {
		if (error) {
			*error = QStringLiteral(
				"The stream resolution must be even, fit the Main video input, and be applied before the encoder starts.");
		}
		return false;
	}
	if (obs_encoder_active(encoder)) {
		if (obs_encoder_get_width(encoder) == width && obs_encoder_get_height(encoder) == height) {
			return true;
		}
		if (error) {
			*error = QStringLiteral("Main streaming encoder is active with a different resolution.");
		}
		return false;
	}
	obs_encoder_set_scaled_size(encoder, width, height);
	obs_encoder_set_gpu_scale_type(encoder, OBS_SCALE_BICUBIC);
	return true;
}

bool ResolveStudioReplayPreset(OBSBasic *main, StudioCanvasTarget canvas, obs_encoder_t *streamVideo,
			       const std::vector<obs_encoder_t *> &streamAudio, bool streamIsEffective,
			       StudioOutputPreset &result, QString &error)
{
	error.clear();
	if (!main || !streamVideo) {
		error = QStringLiteral("Streaming encoder template is unavailable.");
		return false;
	}
	auto *config = main->Config();
	const QString prefix = canvas == StudioCanvasTarget::Main ? "Main" : "Portrait";
	const QString source = QString::fromUtf8(
		config_get_string(config, "StudioCanvas", (prefix + "ReplayPresetSource").toUtf8().constData()));
	const bool custom = source == "custom";
	if (!custom && (config_get_bool(config, "Stream1", "EnableMultitrackVideo") ||
			config_get_int(config, "Stream1", "WHIPSimulcastTotalLayers") > 1)) {
		error = QStringLiteral(
			"Follow stream is unavailable for Enhanced Broadcasting or WHIP simulcast. Select a custom clip preset.");
		return false;
	}
	StudioOutputPreset plan;
	plan.canvas = canvas;
	plan.origin = custom ? "custom" : streamIsEffective ? "stream-effective" : "stream-offline";
	plan.encoderId = QString::fromUtf8(obs_encoder_get_id(streamVideo));
	plan.videoSettings = EncoderData(streamVideo);
	plan.video = obs_encoder_parent_video(streamVideo);
	if (!plan.video) {
		plan.video = obs_get_video();
	}
	plan.width = obs_encoder_get_width(streamVideo);
	plan.height = obs_encoder_get_height(streamVideo);
	if (!plan.width && plan.video) {
		plan.width = video_output_get_width(plan.video);
	}
	if (!plan.height && plan.video) {
		plan.height = video_output_get_height(plan.video);
	}
	plan.fpsDivisor = std::max<uint32_t>(1, obs_encoder_get_frame_rate_divisor(streamVideo));
	plan.preferredFormat = obs_encoder_get_preferred_video_format(streamVideo);
	plan.scaleType = obs_encoder_get_scale_type(streamVideo);
	if (!streamIsEffective) {
		OfflineVideo(main, streamVideo, plan.videoSettings);
		if (canvas == StudioCanvasTarget::Main) {
			if (plan.video) {
				plan.width = video_output_get_width(plan.video);
				plan.height = video_output_get_height(plan.video);
			}
			QString resolution = Resolution(config);
			if (!resolution.isEmpty()) {
				// Match ApplyStudioMainStreamScale, including its GPU scaling
				// policy. Dimensions alone would silently select CPU conversion.
				plan.scaleType = OBS_SCALE_BICUBIC;
			} else if (QString::fromUtf8(config_get_string(config, "Output", "Mode")) == "Advanced") {
				plan.scaleType =
					static_cast<obs_scale_type>(config_get_int(config, "AdvOut", "RescaleFilter"));
				if (plan.scaleType != OBS_SCALE_DISABLE) {
					resolution =
						QString::fromUtf8(config_get_string(config, "AdvOut", "RescaleRes"));
				}
			}
			if (!resolution.isEmpty() && !Size(resolution, plan.width, plan.height)) {
				error = QStringLiteral("Invalid Main stream resolution.");
				return false;
			}
		} else {
			plan.width = 1080;
			plan.height = 1920;
			// The future Portrait stream encodes the native Portrait canvas
			// without Main's per-encoder downscale (PreparePortraitPipeline).
			plan.scaleType = OBS_SCALE_DISABLE;
			const QString id = PortraitH264(plan.encoderId);
			if (id.isEmpty()) {
				error = QStringLiteral("No H.264 encoder is available for Portrait.");
				return false;
			}
			if (id != plan.encoderId) {
				plan.encoderId = id;
				OBSDataAutoRelease defaults = obs_encoder_defaults(id.toUtf8().constData());
				plan.videoSettings = Freeze(defaults);
			}
			const auto bitrate = config_get_int(config, "StudioCanvas", "PortraitBitrate");
			obs_data_set_string(plan.videoSettings, "rate_control", "CBR");
			obs_data_set_int(plan.videoSettings, "bitrate",
					 bitrate > 0 ? std::clamp<int64_t>(bitrate, 100, 100000) : 6000);
			obs_data_set_int(plan.videoSettings, "keyint_sec", 2);
		}
	}
	for (auto *encoder : streamAudio) {
		if (!encoder) {
			continue;
		}
		StudioAudioPreset track;
		track.encoderId = QString::fromUtf8(obs_encoder_get_id(encoder));
		track.settings = EncoderData(encoder);
		track.mixerIndex = obs_encoder_get_mixer_index(encoder);
		if (!streamIsEffective) {
			const bool simple = QString::fromUtf8(config_get_string(config, "Output", "Mode")) !=
					    "Advanced";
			int bitrate = simple ? int(config_get_int(config, "SimpleOutput", "ABitrate"))
					     : int(config_get_int(config, "AdvOut",
								  QString("Track%1Bitrate")
									  .arg(track.mixerIndex + 1)
									  .toUtf8()
									  .constData()));
			bitrate = FindClosestAvailableAudioBitrate(track.encoderId.toUtf8().constData(), bitrate);
			obs_data_set_int(track.settings, "bitrate", bitrate);
			if ((simple || config_get_bool(config, "AdvOut", "ApplyServiceSettings")) &&
			    main->GetService()) {
				obs_service_apply_encoder_settings(main->GetService(), nullptr, track.settings);
				if (config_get_bool(config, "Stream1", "IgnoreRecommended")) {
					obs_data_set_int(track.settings, "bitrate", bitrate);
				}
			}
		}
		plan.audio.push_back(std::move(track));
	}
	if (custom) {
		const int64_t bitrate =
			config_get_int(config, "StudioCanvas", (prefix + "ReplayBitrate").toUtf8().constData());
		obs_data_set_string(plan.videoSettings, "rate_control", "CBR");
		obs_data_set_int(plan.videoSettings, "bitrate",
				 bitrate > 0 ? std::clamp<int64_t>(bitrate, 100, 100000) : 6000);
		const auto resolution = QString::fromUtf8(
			config_get_string(config, "StudioCanvas", (prefix + "ReplayResolution").toUtf8().constData()));
		if (!resolution.isEmpty() && !Size(resolution, plan.width, plan.height)) {
			error = QStringLiteral("Invalid custom clip resolution.");
			return false;
		}
		const auto audioBitrate =
			config_get_int(config, "StudioCanvas", (prefix + "ReplayAudioBitrate").toUtf8().constData());
		if (audioBitrate > 0) {
			for (auto &track : plan.audio) {
				obs_data_set_int(track.settings, "bitrate",
						 FindClosestAvailableAudioBitrate(track.encoderId.toUtf8().constData(),
										  int(audioBitrate)));
			}
		}
	}
	const uint32_t inputWidth = canvas == StudioCanvasTarget::Portrait ? 1080
				    : plan.video                           ? video_output_get_width(plan.video)
									   : 0;
	const uint32_t inputHeight = canvas == StudioCanvasTarget::Portrait ? 1920
				     : plan.video                           ? video_output_get_height(plan.video)
									    : 0;
	if (custom && plan.scaleType == OBS_SCALE_DISABLE && (plan.width < inputWidth || plan.height < inputHeight)) {
		// A custom downscale needs its own GPU scale even when the stream
		// template is unscaled. Preserve an explicitly inherited live filter.
		plan.scaleType = OBS_SCALE_BICUBIC;
	}
	if (!plan.width || !plan.height || plan.width > inputWidth || plan.height > inputHeight || plan.audio.empty()) {
		error = QStringLiteral(
			"Clip dimensions exceed the canvas video input, or its selected stream audio mix is unavailable.");
		return false;
	}
	result = std::move(plan);
	return true;
}
