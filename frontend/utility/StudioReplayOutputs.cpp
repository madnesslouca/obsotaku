#include "StudioReplayOutputs.hpp"
#include "BasicOutputHandler.hpp"
#include "StudioOutputPreset.hpp"
#include "StudioReplayTemplate.hpp"
#include "DualCanvasController.hpp"
#include <widgets/OBSBasic.hpp>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <atomic>

struct StudioReplayOutputs::Buffer {
	StudioReplayOutputs *owner;
	QString canvas;
	OBSCanvas retainedCanvas;
	StudioOutputPreset preset;
	OBSEncoderAutoRelease video;
	std::array<OBSEncoderAutoRelease, MAX_AUDIO_MIXES> audio;
	OBSOutputAutoRelease output;
	OBSSignal stopped, saved;
	std::atomic<uint64_t> generation{0};
	bool running = false, stopping = false;
	bool stopIssued = false, stopPollQueued = false, stopCancelTried = false;
	int64_t stopSaveId = 0;
	QElapsedTimer stopSaveClock;
	int64_t requestId = 0;
	QString pairId, lastError, lastPath;
	Buffer(StudioReplayOutputs *owner_, QString canvas_) : owner(owner_), canvas(std::move(canvas_)) {}
	bool Saving() const
	{
		if (!output) {
			return false;
		}
		calldata_t cd = {0};
		proc_handler_call(obs_output_get_proc_handler(output), "get_replay_status", &cd);
		const bool saving = calldata_bool(&cd, "saving");
		calldata_free(&cd);
		return saving;
	}
	void Release()
	{
		stopped.Disconnect();
		saved.Disconnect();
		output = nullptr;
		video = nullptr;
		for (auto &track : audio) {
			track = nullptr;
		}
		retainedCanvas = nullptr;
	}
};

namespace {
bool Selected(const QString &targets, const QString &canvas)
{
	return targets == "both" || targets == canvas;
}
bool ValidTargets(const QString &targets)
{
	return targets == "both" || targets == "main" || targets == "portrait";
}
} // namespace

StudioReplayOutputs::StudioReplayOutputs(BasicOutputHandler *handler_, OBSBasic *main_)
	: QObject(main_),
	  handler(handler_),
	  main(main_)
{
	setObjectName("studioReplayOutputs");
	buffers[0] = std::make_unique<Buffer>(this, "main");
	buffers[1] = std::make_unique<Buffer>(this, "portrait");
}

StudioReplayOutputs::~StudioReplayOutputs()
{
	closing = true;
	for (auto &buffer : buffers) {
		buffer->stopped.Disconnect();
		buffer->saved.Disconnect();
		// Never force-stop an output which failed to start (QA-006).
		if (buffer->output && obs_output_active(buffer->output)) {
			obs_output_force_stop(buffer->output);
		}
		// Output destruction joins its mux worker before releasing its encoders.
		buffer->Release();
	}
}

bool StudioReplayOutputs::Active() const
{
	for (const auto &buffer : buffers) {
		if (buffer->running || buffer->Saving() || (buffer->output && obs_output_active(buffer->output))) {
			return true;
		}
	}
	return false;
}

OBSOutputAutoRelease StudioReplayOutputs::PreferredOutput() const
{
	// The legacy API exposes one handle. Prefer Main still capturing, then a
	// healthy Portrait, before returning a stopping or configured idle handle.
	for (const auto &buffer : buffers) {
		if (buffer->output && !buffer->stopping && obs_output_active(buffer->output)) {
			return obs_output_get_ref(buffer->output);
		}
	}
	for (const auto &buffer : buffers) {
		if (buffer->output && (buffer->running || obs_output_active(buffer->output))) {
			return obs_output_get_ref(buffer->output);
		}
	}
	for (const auto &buffer : buffers) {
		if (buffer->output) {
			return obs_output_get_ref(buffer->output);
		}
	}
	return {};
}

bool StudioReplayOutputs::Prepare(Buffer &buffer, QString &error)
{
	buffer.Release();
	++buffer.generation;
	const bool portrait = buffer.canvas == "portrait";
	const auto target = portrait ? StudioCanvasTarget::Portrait : StudioCanvasTarget::Main;
	StudioOutputPreset plan;
	if (!ResolveStudioReplayTemplate(handler, target, plan, error)) {
		return false;
	}
	if (portrait) {
		auto *controller = main->findChild<DualCanvasController *>();
		buffer.retainedCanvas = controller ? controller->PortraitCanvas() : OBSCanvas{};
		if (!buffer.retainedCanvas || !obs_canvas_get_video(buffer.retainedCanvas)) {
			error = QStringLiteral("Enable Multi Canvas before starting the Portrait clip buffer.");
			return false;
		}
		plan.video = obs_canvas_get_video(buffer.retainedCanvas);
	}
	if (!plan.video || plan.width > video_output_get_width(plan.video) ||
	    plan.height > video_output_get_height(plan.video)) {
		error = QStringLiteral("Clip resolution exceeds the selected canvas video input.");
		return false;
	}
	auto *config = main->Config();
	const bool advanced = QString::fromUtf8(config_get_string(config, "Output", "Mode")) == "Advanced";
	const char *section = advanced ? "AdvOut" : "SimpleOutput";
	const QString directory =
		QString::fromUtf8(config_get_string(config, section, advanced ? "RecFilePath" : "FilePath"));
	const QFileInfo dir(directory);
	if (directory.isEmpty() || !dir.isDir() || !dir.isWritable()) {
		error = QStringLiteral("The clip directory does not exist or is not writable.");
		return false;
	}
	const auto name = QString("studio_%1_replay").arg(buffer.canvas).toUtf8();
	buffer.video = obs_video_encoder_create(plan.encoderId.toUtf8().constData(), name.constData(),
						plan.videoSettings, nullptr);
	if (!buffer.video) {
		error = QStringLiteral("Could not create the clip video encoder.");
		return false;
	}
	obs_encoder_set_video(buffer.video, plan.video);
	obs_encoder_set_scaled_size(buffer.video, plan.width, plan.height);
	obs_encoder_set_gpu_scale_type(buffer.video, plan.scaleType);
	obs_encoder_set_frame_rate_divisor(buffer.video, plan.fpsDivisor);
	obs_encoder_set_preferred_video_format(buffer.video, plan.preferredFormat);
	for (size_t i = 0; i < plan.audio.size() && i < MAX_AUDIO_MIXES; ++i) {
		const auto &track = plan.audio[i];
		buffer.audio[i] = obs_audio_encoder_create(track.encoderId.toUtf8().constData(),
							   (name + "_audio_" + QByteArray::number(i)).constData(),
							   track.settings, track.mixerIndex, nullptr);
		if (!buffer.audio[i]) {
			error = QStringLiteral("Could not create a selected clip audio encoder.");
			return false;
		}
		obs_encoder_set_audio(buffer.audio[i], obs_get_audio());
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "directory", directory.toUtf8().constData());
	QString format = QString::fromUtf8(config_get_string(config, "Output", "FilenameFormatting"));
	if (format.isEmpty()) {
		format = "%CCYY-%MM-%DD %hh-%mm-%ss";
	}
	format = QString::fromUtf8(config_get_string(config, "SimpleOutput", "RecRBPrefix")) + format +
		 QString::fromUtf8(config_get_string(config, "SimpleOutput", "RecRBSuffix")) + "-" + buffer.canvas;
	obs_data_set_string(settings, "format", format.toUtf8().constData());
	// Matroska preserves all selected audio mixes and survives interrupted saves.
	obs_data_set_string(settings, "extension", "mkv");
	obs_data_set_bool(settings, "allow_spaces", true);
	// Canonical ReplayBuffer.Save is routed by OBSBasic to SaveTargets(both).
	obs_data_set_bool(settings, "studio_disable_save_hotkey", true);
	obs_data_set_int(settings, "max_time_sec",
			 std::clamp<int64_t>(config_get_int(config, section, "RecRBTime"), 1, 3600));
	const auto size = config_get_int(config, section, "RecRBSize");
	obs_data_set_int(settings, "max_size_mb", size > 0 ? std::clamp<int64_t>(size, 1, 8192) : 512);
	// One frontend shortcut coordinates both canvases and their shared pair ID.
	// Per-output shortcuts would issue independent, unpaired save requests.
	buffer.output = obs_output_create("replay_buffer", name.constData(), settings, nullptr);
	if (!buffer.output) {
		error = QStringLiteral("Replay output is unavailable.");
		return false;
	}
	obs_output_set_video_encoder(buffer.output, buffer.video);
	for (size_t i = 0; i < plan.audio.size() && i < MAX_AUDIO_MIXES; ++i) {
		obs_output_set_audio_encoder(buffer.output, buffer.audio[i], i);
	}
	buffer.preset = std::move(plan);
	buffer.stopIssued = buffer.stopPollQueued = buffer.stopCancelTried = false;
	buffer.stopSaveId = 0;
	buffer.stopSaveClock.invalidate();
	buffer.requestId = 0;
	buffer.pairId.clear();
	buffer.stopped.Connect(obs_output_get_signal_handler(buffer.output), "stop", OnStop, &buffer);
	buffer.saved.Connect(obs_output_get_signal_handler(buffer.output), "save_result", OnSaveResult, &buffer);
	return true;
}

bool StudioReplayOutputs::StartTargets(QString targets)
{
	if (closing || !ValidTargets(targets)) {
		return false;
	}
	std::vector<Buffer *> prepared;
	// Validate both plans before starting either new buffer. Existing healthy
	// sessions are not rebuilt or stopped by a failed additional request.
	for (auto &buffer : buffers) {
		if (!Selected(targets, buffer->canvas) || (buffer->running && !buffer->stopping)) {
			continue;
		}
		QString error;
		if (buffer->stopping || buffer->Saving()) {
			error = QStringLiteral("This clip buffer is still stopping or saving.");
		} else if (Prepare(*buffer, error)) {
			prepared.push_back(buffer.get());
			continue;
		}
		buffer->lastError = error;
		emit Error(buffer->canvas, error);
		for (auto *candidate : prepared) {
			candidate->Release();
		}
		NotifyAggregate();
		return false;
	}
	bool complete = true;
	for (auto *buffer : prepared) {
		if (obs_output_start(buffer->output)) {
			buffer->running = true;
			buffer->stopping = false;
			buffer->lastError.clear();
			continue;
		}
		const char *detail = obs_output_get_last_error(buffer->output);
		buffer->lastError = detail && *detail ? QString::fromUtf8(detail)
						      : QStringLiteral("Clip encoder could not start.");
		emit Error(buffer->canvas, buffer->lastError);
		complete = false;
		// A late encoder/start failure is a partial result. Other buffers keep
		// capturing, including a sibling started by this very request.
	}
	NotifyAggregate();
	return complete;
}

QJsonObject StudioReplayOutputs::SaveTargets(QString targets)
{
	QJsonObject result;
	if (closing || !ValidTargets(targets)) {
		return {{"error", "Invalid clip target"}};
	}
	const QString pairId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	for (auto &buffer : buffers) {
		if (!Selected(targets, buffer->canvas)) {
			continue;
		}
		calldata_t cd = {0};
		if (buffer->stopping) {
			calldata_set_string(&cd, "error", "Clip buffer is stopping; no new save request was accepted.");
		} else if (buffer->output) {
			proc_handler_call(obs_output_get_proc_handler(buffer->output), "save_request", &cd);
		}
		const bool accepted = calldata_bool(&cd, "accepted");
		QString error = buffer->output ? QString::fromUtf8(calldata_string(&cd, "error"))
					       : QStringLiteral("Clip buffer is not running.");
		const auto id = calldata_int(&cd, "request_id");
		if (accepted) {
			buffer->requestId = id;
			buffer->pairId = pairId;
			buffer->lastError.clear();
			const auto generation = buffer->generation.load();
			auto *pending = buffer.get();
			QTimer::singleShot(10000, this, [this, pending, generation, id]() {
				if (closing || pending->generation != generation || pending->requestId != id ||
				    !pending->Saving()) {
					return;
				}
				// An encoder can stall before delivering the next decodable packet.
				// Cancel only that pending request; a mux worker owns its snapshot.
				calldata_t cancel = {0};
				calldata_set_int(&cancel, "request_id", id);
				proc_handler_call(obs_output_get_proc_handler(pending->output), "cancel_save", &cancel);
				calldata_free(&cancel);
			});
		} else {
			buffer->lastError = error;
			if (targets != "both" || buffer->running) {
				emit Error(buffer->canvas, error);
			}
		}
		result.insert(buffer->canvas, QJsonObject{{"accepted", accepted},
							  {"request_id", qint64(id)},
							  {"pair_id", pairId},
							  {"error", error}});
		calldata_free(&cd);
	}
	return result;
}

void StudioReplayOutputs::StopTargets(QString targets, bool force)
{
	if (closing || !ValidTargets(targets)) {
		return;
	}
	for (auto &buffer : buffers) {
		if (!Selected(targets, buffer->canvas) || !buffer->running) {
			continue;
		}
		buffer->stopping = true;
		RequestStop(*buffer, force);
	}
	NotifyAggregate();
}

void StudioReplayOutputs::RequestStop(Buffer &buffer, bool force)
{
	if (closing || !buffer.output || !buffer.stopping || !buffer.running) {
		return;
	}
	if (force) {
		// Cancel only a pending pre-snapshot request with its exact ID. A mux
		// worker already writing owns its packet snapshot and must finish safely.
		calldata_t status = {0};
		proc_handler_call(obs_output_get_proc_handler(buffer.output), "get_replay_status", &status);
		calldata_t cancel = {0};
		calldata_set_int(&cancel, "request_id", calldata_int(&status, "request_id"));
		proc_handler_call(obs_output_get_proc_handler(buffer.output), "cancel_save", &cancel);
		calldata_free(&cancel);
		calldata_free(&status);
		buffer.stopIssued = true;
		if (obs_output_active(buffer.output)) {
			obs_output_force_stop(buffer.output);
		} else {
			CompleteStop(buffer, OBS_OUTPUT_SUCCESS, {});
		}
		return;
	}
	if (buffer.stopIssued) {
		return;
	}
	calldata_t status = {0};
	proc_handler_call(obs_output_get_proc_handler(buffer.output), "get_replay_status", &status);
	const bool saving = calldata_bool(&status, "saving");
	const auto id = calldata_int(&status, "request_id");
	calldata_free(&status);
	if (saving) {
		if (!buffer.stopSaveClock.isValid() || id != buffer.stopSaveId) {
			buffer.stopSaveId = id;
			buffer.stopSaveClock.start();
			buffer.stopCancelTried = false;
		}
		if (!buffer.stopCancelTried && buffer.stopSaveClock.elapsed() >= 10000) {
			buffer.stopCancelTried = true;
			// Also bound a pre-snapshot request made through the singular legacy
			// output proc, which did not pass through SaveTargets' own timer.
			calldata_t cancel = {0};
			calldata_set_int(&cancel, "request_id", id);
			proc_handler_call(obs_output_get_proc_handler(buffer.output), "cancel_save", &cancel);
			calldata_free(&cancel);
		}
		if (!buffer.stopPollQueued) {
			buffer.stopPollQueued = true;
			const auto generation = buffer.generation.load();
			QTimer::singleShot(20, this, [this, &buffer, generation]() {
				if (closing || buffer.generation != generation) {
					return;
				}
				buffer.stopPollQueued = false;
				RequestStop(buffer, false);
			});
		}
		return;
	}
	buffer.stopIssued = true;
	if (obs_output_active(buffer.output)) {
		obs_output_stop(buffer.output);
	} else {
		CompleteStop(buffer, OBS_OUTPUT_SUCCESS, {});
	}
}

void StudioReplayOutputs::NotifyAggregate()
{
	if (closing) {
		return;
	}
	bool anyRunning = false, allStopping = true;
	for (const auto &buffer : buffers) {
		if (!buffer->running) {
			continue;
		}
		anyRunning = true;
		allStopping = allStopping && buffer->stopping;
	}
	const bool stopping = anyRunning && allStopping;
	if (stopping != notifiedStopping) {
		notifiedStopping = stopping;
		emit AggregateStoppingChanged(stopping);
		if (stopping) {
			emit AggregateStopping();
		}
	}
	const bool active = Active();
	if (active == notifiedActive) {
		return;
	}
	notifiedActive = active;
	emit AggregateChanged(active);
}

void StudioReplayOutputs::OnStop(void *data, calldata_t *params)
{
	auto *buffer = static_cast<Buffer *>(data);
	const int code = int(calldata_int(params, "code"));
	const QString error = QString::fromUtf8(calldata_string(params, "last_error"));
	const auto generation = buffer->generation.load();
	QMetaObject::invokeMethod(
		buffer->owner,
		[buffer, generation, code, error]() {
			if (buffer->generation == generation && !buffer->owner->closing) {
				buffer->owner->CompleteStop(*buffer, code, error);
			}
		},
		Qt::QueuedConnection);
}

void StudioReplayOutputs::CompleteStop(Buffer &buffer, int code, QString error)
{
	buffer.stopping = true;
	if (obs_output_active(buffer.output) || buffer.Saving()) {
		const auto generation = buffer.generation.load();
		QTimer::singleShot(20, this, [this, &buffer, generation, code, error]() {
			if (buffer.generation == generation && !closing) {
				CompleteStop(buffer, code, error);
			}
		});
		return;
	}
	buffer.running = false;
	buffer.stopping = false;
	if (code != OBS_OUTPUT_SUCCESS) {
		buffer.lastError = error.isEmpty() ? QString("Clip buffer stopped with error %1").arg(code) : error;
		emit Error(buffer.canvas, buffer.lastError);
	}
	NotifyAggregate();
}

void StudioReplayOutputs::OnSaveResult(void *data, calldata_t *params)
{
	auto *buffer = static_cast<Buffer *>(data);
	const auto generation = buffer->generation.load();
	const auto id = calldata_int(params, "request_id");
	const bool success = calldata_bool(params, "success");
	const QString path = QString::fromUtf8(calldata_string(params, "path"));
	const QString error = QString::fromUtf8(calldata_string(params, "error"));
	QMetaObject::invokeMethod(
		buffer->owner,
		[buffer, generation, id, success, path, error]() {
			auto *owner = buffer->owner;
			if (owner->closing || buffer->generation != generation) {
				return;
			}
			const QString pair = id == buffer->requestId
						     ? buffer->pairId
						     : QUuid::createUuid().toString(QUuid::WithoutBraces);
			if (success) {
				buffer->lastPath = path;
				buffer->lastError.clear();
				emit owner->Saved(buffer->canvas, path, pair);
			} else {
				buffer->lastError = error;
				emit owner->Error(buffer->canvas, error);
			}
			if (buffer->stopping) {
				owner->RequestStop(*buffer, false);
			}
			owner->NotifyAggregate();
		},
		Qt::QueuedConnection);
}

QJsonObject StudioReplayOutputs::Snapshot() const
{
	QJsonObject states;
	for (const auto &buffer : buffers) {
		QJsonObject state{{"active", buffer->running},    {"stopping", buffer->stopping},
				  {"saving", buffer->Saving()},   {"lastError", buffer->lastError},
				  {"lastPath", buffer->lastPath}, {"preset", buffer->preset.Summary()}};
		if (buffer->output) {
			calldata_t cd = {0};
			proc_handler_call(obs_output_get_proc_handler(buffer->output), "get_replay_status", &cd);
			for (const auto *key : {"retained_bytes", "snapshot_bytes", "limit_bytes", "request_id"}) {
				state.insert(key, qint64(calldata_int(&cd, key)));
			}
			calldata_free(&cd);
		}
		states.insert(buffer->canvas, state);
	}
	return {{"active", Active()}, {"buffers", states}};
}
