#include "StudioCanvasOutputs.hpp"
#include "DualCanvasController.hpp"
#include <obs-frontend-api.h>
#include <qt-wrappers.hpp>
#include <OBSApp.hpp>
#include <util/config-file.h>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <array>

struct StudioCanvasOutputs::Session : QObject {
	StudioCanvasOutputs *owner = nullptr;
	OBSOutputAutoRelease primary, extra;
	OBSCanvas canvas;
	OBSEncoderAutoRelease originalVideo, portraitVideo, primaryVideo;
	std::array<OBSEncoderAutoRelease, MAX_AUDIO_MIXES> originalAudio, primaryAudio, extraAudio;
	void Restore()
	{
		if (!primary || obs_output_active(primary)) {
			return;
		}
		obs_output_set_video_encoder(primary, originalVideo);
		for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
			obs_output_set_audio_encoder(primary, originalAudio[i], i);
		}
	}
	~Session() { Restore(); }
	OBSSignal extraStop, stopping, paused, unpaused;
	std::function<void(int, QString)> complete;
	bool primaryStopped = false, extraStopped = true;
	int code = OBS_OUTPUT_SUCCESS;
	QString error;
};

StudioCanvasOutputs::StudioCanvasOutputs(QObject *parent) : QObject(parent) {}
StudioCanvasOutputs::~StudioCanvasOutputs() = default;

bool StudioCanvasOutputs::Start(obs_output_t *output, bool isReplay, QString &error)
{
	auto &slot = isReplay ? replay : recording;
	if (slot && (obs_output_active(slot->primary) || (slot->extra && obs_output_active(slot->extra)))) {
		error = QTStr("Studio.Canvas.Error.Stopping");
		return false;
	}
	slot.reset();
	auto *config = obs_frontend_get_profile_config();
	const QString mode = QString::fromUtf8(
		config_get_string(config, "StudioCanvas", isReplay ? "ReplayCanvas" : "RecordingCanvas"));
	if (mode.isEmpty() || mode == "main") {
		return obs_output_start(output);
	}
	if ((mode != "portrait" && mode != "both") || (isReplay && mode == "both")) {
		error = QTStr("Studio.Canvas.Error.Invalid");
		return false;
	}
	if (!output || !(obs_output_get_flags(output) & OBS_OUTPUT_ENCODED) ||
	    QString::fromUtf8(obs_output_get_id(output)) == "ffmpeg_output") {
		error = QTStr("Studio.Canvas.Error.EncodedFormat");
		return false;
	}
	auto *controller = parent()->findChild<DualCanvasController *>();
	OBSCanvas canvas = controller ? controller->PortraitCanvas() : OBSCanvas{};
	if (!canvas || !obs_canvas_get_video(canvas)) {
		error = QTStr("Studio.Canvas.Error.EnableMulti");
		return false;
	}
	auto session = std::make_unique<Session>();
	session->owner = this;
	session->canvas = canvas;
	session->primary = obs_output_get_ref(output);
	session->originalVideo = obs_encoder_get_ref(obs_output_get_video_encoder(output));
	for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
		session->originalAudio[i] = obs_encoder_get_ref(obs_output_get_audio_encoder(output, i));
	}
	if (!session->originalVideo) {
		error = QTStr("Studio.Canvas.Error.EncoderNotReady");
		return false;
	}
	const QString codec = QString::fromUtf8(obs_encoder_get_codec(session->originalVideo));
	if (codec != "h264" && codec != "hevc" && codec != "av1") {
		error = QTStr("Studio.Canvas.Error.Codec");
		return false;
	}
	const char *id = obs_encoder_get_id(session->originalVideo);
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease originalSettings = obs_encoder_get_settings(session->originalVideo);
	obs_data_apply(settings, originalSettings);
	int64_t bitrate = config_get_int(config, "StudioCanvas", "PortraitRecordingBitrate");
	bitrate = bitrate > 0 ? std::clamp<int64_t>(bitrate, 100, 100000) : 6000;
	// CBR is deliberate: the portrait bitrate never changes a shared stream/record encoder.
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "bitrate", bitrate);
	obs_data_set_int(settings, "keyint_sec", 2);
	session->portraitVideo = obs_video_encoder_create(
		id, isReplay ? "studio_portrait_replay" : "studio_portrait_recording", settings, nullptr);
	if (!session->portraitVideo) {
		error = QTStr("Studio.Canvas.Error.VideoEncoder");
		return false;
	}
	obs_encoder_set_video(session->portraitVideo, obs_canvas_get_video(canvas));
	obs_encoder_set_frame_rate_divisor(session->portraitVideo,
					   obs_encoder_get_frame_rate_divisor(session->originalVideo));
	obs_encoder_set_preferred_video_format(session->portraitVideo,
					       obs_encoder_get_preferred_video_format(session->originalVideo));
	if (mode == "both") {
		// A replay buffer may still use the native main encoder; pausing this
		// recording must not pause that replay or an active stream.
		session->primaryVideo =
			obs_video_encoder_create(id, "studio_main_recording", originalSettings, nullptr);
		if (!session->primaryVideo) {
			error = QTStr("Studio.Canvas.Error.VideoEncoder");
			return false;
		}
		obs_encoder_set_video(session->primaryVideo, obs_encoder_parent_video(session->originalVideo));
		obs_encoder_set_preferred_video_format(session->primaryVideo,
						       obs_encoder_get_preferred_video_format(session->originalVideo));
		obs_encoder_set_frame_rate_divisor(session->primaryVideo,
						   obs_encoder_get_frame_rate_divisor(session->originalVideo));
		if (obs_encoder_scaling_enabled(session->originalVideo)) {
			obs_encoder_set_scaled_size(session->primaryVideo,
						    obs_encoder_get_width(session->originalVideo),
						    obs_encoder_get_height(session->originalVideo));
			obs_encoder_set_gpu_scale_type(session->primaryVideo,
						       obs_encoder_get_scale_type(session->originalVideo));
		}
	}

	// Independent audio encoders keep Pause from muting a stream or a clip that
	// shares the native recording preset. Mixer indices and codec settings stay identical.
	for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
		auto *original = session->originalAudio[i].Get();
		if (!original) {
			continue;
		}
		OBSDataAutoRelease audioSettings = obs_encoder_get_settings(original);
		const std::string name =
			std::string(isReplay ? "studio_clip_audio_" : "studio_record_audio_") + std::to_string(i);
		session->primaryAudio[i] = obs_audio_encoder_create(obs_encoder_get_id(original), name.c_str(),
								    audioSettings,
								    obs_encoder_get_mixer_index(original), nullptr);
		if (mode == "both") {
			session->extraAudio[i] =
				obs_audio_encoder_create(obs_encoder_get_id(original), (name + "_portrait").c_str(),
							 audioSettings, obs_encoder_get_mixer_index(original), nullptr);
		}
		if (!session->primaryAudio[i] || (mode == "both" && !session->extraAudio[i])) {
			error = QTStr("Studio.Canvas.Error.AudioEncoder");
			return false;
		}
		obs_encoder_set_name(session->primaryAudio[i], obs_encoder_get_name(original));
		obs_encoder_set_audio(session->primaryAudio[i], obs_get_audio());
		if (session->extraAudio[i]) {
			obs_encoder_set_name(session->extraAudio[i], obs_encoder_get_name(original));
			obs_encoder_set_audio(session->extraAudio[i], obs_get_audio());
		}
	}
	for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
		obs_output_set_audio_encoder(output, session->primaryAudio[i], i);
	}
	if (mode == "portrait") {
		obs_output_set_video_encoder(output, session->portraitVideo);
	} else {
		OBSDataAutoRelease outputSettings = obs_data_create();
		OBSDataAutoRelease originalOutputSettings = obs_output_get_settings(output);
		obs_data_apply(outputSettings, originalOutputSettings);
		const QString original = QString::fromUtf8(obs_data_get_string(outputSettings, "path"));
		if (original.isEmpty()) {
			error = QTStr("Studio.Canvas.Error.Path");
			return false;
		}
		QFileInfo file(original);
		QString name = file.completeBaseName() + "-portrait";
		QString path = file.dir().filePath(name + "." + file.suffix());
		for (int n = 2; QFileInfo::exists(path); ++n) {
			path = file.dir().filePath(name + QString("-%1.").arg(n) + file.suffix());
		}
		obs_data_set_string(outputSettings, "path", path.toUtf8().constData());
		// Native split-file outputs use format instead of path after the first segment.
		const QString format = QString::fromUtf8(obs_data_get_string(outputSettings, "format"));
		if (!format.isEmpty()) {
			obs_data_set_string(outputSettings, "format", (format + "-portrait").toUtf8().constData());
		}
		session->extra = obs_output_create(obs_output_get_id(output), "studio_portrait_recording",
						   outputSettings, nullptr);
		if (!session->extra) {
			error = QTStr("Studio.Canvas.Error.Output");
			return false;
		}
		obs_output_set_video_encoder(session->extra, session->portraitVideo);
		obs_output_set_video_encoder(output, session->primaryVideo);
		for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
			obs_output_set_audio_encoder(session->extra, session->extraAudio[i], i);
		}
		Session *s = session.get();
		s->extraStopped = false;
		s->extraStop.Connect(
			obs_output_get_signal_handler(s->extra), "stop",
			[](void *data, calldata_t *params) {
				auto *s = static_cast<Session *>(data);
				const int code = int(calldata_int(params, "code"));
				const QString error = QString::fromUtf8(calldata_string(params, "last_error"));
				QMetaObject::invokeMethod(
					s,
					[s, code, error]() {
						s->extraStopped = true;
						if (code != OBS_OUTPUT_SUCCESS) {
							s->code = code;
							s->error = error.isEmpty() ? QTStr("Studio.Canvas.Error.Failed")
										   : error;
							if (obs_output_active(s->primary)) {
								obs_output_stop(s->primary);
							}
						}
						s->owner->TryFinish(*s);
					},
					Qt::QueuedConnection);
			},
			s);
		s->stopping.Connect(
			obs_output_get_signal_handler(output), "stopping",
			[](void *data, calldata_t *) {
				auto *s = static_cast<Session *>(data);
				QMetaObject::invokeMethod(
					s,
					[s]() {
						if (obs_output_active(s->extra)) {
							obs_output_stop(s->extra);
						}
					},
					Qt::QueuedConnection);
			},
			s);
		if (!obs_output_initialize_encoders(output, 0) || !obs_output_initialize_encoders(s->extra, 0)) {
			error = QTStr("Studio.Canvas.Error.VideoEncoder");
			return false;
		}
		// Warm both encoders before capture, avoiding a hardware initialization delay between files.
		if (!obs_output_start(s->extra)) {
			error = QString::fromUtf8(obs_output_get_last_error(s->extra));
			if (error.isEmpty()) {
				error = QTStr("Studio.Canvas.Error.Start");
			}
			return false;
		}
		// OBS signals pause on the native output for both hotkeys and frontend APIs.
		s->paused.Connect(
			obs_output_get_signal_handler(output), "pause",
			[](void *data, calldata_t *) {
				auto *s = static_cast<Session *>(data);
				obs_output_pause(s->extra, true);
			},
			s);
		s->unpaused.Connect(
			obs_output_get_signal_handler(output), "unpause",
			[](void *data, calldata_t *) {
				auto *s = static_cast<Session *>(data);
				obs_output_pause(s->extra, false);
			},
			s);
		blog(LOG_INFO, "Studio: simultaneous portrait recording: %s", path.toUtf8().constData());
	}
	slot = std::move(session);
	if (!obs_output_start(output)) {
		error = QString::fromUtf8(obs_output_get_last_error(output));
		if (slot->extra) {
			obs_output_force_stop(slot->extra);
		}
		slot->Restore();
		return false;
	}
	return true;
}

void StudioCanvasOutputs::Finish(bool isReplay, int code, const QString &error,
				 std::function<void(int, QString)> complete)
{
	QMetaObject::invokeMethod(
		this,
		[this, isReplay, code, error, complete]() {
			auto &slot = isReplay ? replay : recording;
			if (!slot) {
				complete(code, error);
				return;
			}
			slot->primaryStopped = true;
			slot->complete = complete;
			if (code != OBS_OUTPUT_SUCCESS) {
				slot->code = code;
				slot->error = error;
			}
			if (slot->extra && obs_output_active(slot->extra)) {
				obs_output_stop(slot->extra);
			}
			TryFinish(*slot);
		},
		Qt::QueuedConnection);
}

void StudioCanvasOutputs::TryFinish(Session &session)
{
	if (!session.primaryStopped || !session.extraStopped || !session.complete) {
		return;
	}
	// The "stop" signal can arrive before libobs has fully deactivated its encoders.
	if (obs_output_active(session.primary) || (session.extra && obs_output_active(session.extra))) {
		QTimer::singleShot(25, &session, [this, &session]() { TryFinish(session); });
		return;
	}
	session.Restore();
	auto complete = std::move(session.complete);
	complete(session.code, session.error);
}

bool StudioCanvasOutputs::Active() const
{
	return recording && recording->extra && obs_output_active(recording->extra);
}

void StudioCanvasOutputs::StopCompanion(bool force)
{
	if (!recording || !recording->extra || !obs_output_active(recording->extra)) {
		return;
	}
	if (force) {
		obs_output_force_stop(recording->extra);
	} else {
		obs_output_stop(recording->extra);
	}
}
