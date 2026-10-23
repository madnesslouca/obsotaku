/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultiStreamManager.hpp"
#include "MultistreamChannelPlan.hpp"
#include "MultistreamChannelStore.hpp"

#include <util/base.h>
#include <QTimer>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

using namespace std;

namespace {
constexpr uint32_t PORTRAIT_WIDTH = 1080;
constexpr uint32_t PORTRAIT_HEIGHT = 1920;

void WipeSecret(string &value)
{
	volatile char *data = value.empty() ? nullptr : value.data();
	for (size_t index = 0; index < value.size(); ++index) {
		data[index] = 0;
	}
	value.clear();
}

void WipeChannelSecrets(vector<MultiStreamChannel> &channels)
{
	for (auto &channel : channels) {
		WipeSecret(channel.streamKey);
	}
}
} // namespace

MultiStreamManager::~MultiStreamManager()
{
	vector<shared_ptr<Destination>> oldDestinations;
	{
		lock_guard lock(mutex);
		stateCallback = nullptr;
		sessionRunning = false;
		oldDestinations = std::move(destinations);
		for (auto &retired : retiredDestinations) {
			oldDestinations.emplace_back(std::move(retired));
		}
		retiredDestinations.clear();
		active = false;
		WipeChannelSecrets(channels);
	}

	for (const auto &destination : oldDestinations) {
		DisconnectSignals(*destination);
		if (obs_output_active(destination->output) || destination->state == MultiStreamChannelState::Starting) {
			obs_output_force_stop(destination->output);
		}
		WipeSecret(destination->channel.streamKey);
	}
	DestroyPortraitPipeline();
}

bool MultiStreamManager::Configure(vector<MultiStreamChannel> newChannels, string &error)
{
	{
		lock_guard lock(mutex);
		if (active) {
			error = "Multistream channels cannot be changed while an output is active.";
			return false;
		}
	}

	unordered_set<string> ids;
	for (const auto &channel : newChannels) {
		if (channel.id.empty() || channel.displayName.empty()) {
			error = "Every multistream channel needs an id and a display name.";
			return false;
		}
		if (!ids.emplace(channel.id).second) {
			error = "Multistream channel ids must be unique.";
			return false;
		}
		if (channel.enabled && (channel.server.empty() || channel.streamKey.empty())) {
			error = "Every enabled multistream channel needs a server and stream key.";
			return false;
		}
		if (channel.audioMixIndex >= MAX_AUDIO_MIXES ||
		    (channel.vodTrackEnabled && channel.vodTrackIndex >= MAX_AUDIO_MIXES)) {
			error = "A multistream channel selected an audio track that does not exist.";
			return false;
		}
	}

	/* Disconnect the main-canvas callback before taking the manager lock: a
	 * callback already in flight briefly holds that lock while replacing the
	 * portrait source. */
	DestroyPortraitPipeline();
	RetireDestinations();
	{
		lock_guard lock(mutex);
		CollectRetiredDestinations();
		WipeChannelSecrets(channels);
		channels = std::move(newChannels);
	}
	error.clear();
	return true;
}

bool MultiStreamManager::Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
			       const MultiStreamOutputSettings &outputSettings, string &error)
{
	return Start(videoEncoder, defaultAudioEncoder, {}, outputSettings, error);
}

bool MultiStreamManager::Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
			       const vector<obs_encoder_t *> &audioEncodersByTrack,
			       const MultiStreamOutputSettings &outputSettings, string &error)
{
	if (!videoEncoder || !defaultAudioEncoder) {
		error = "Multistream requires initialized video and audio encoders.";
		return false;
	}

	vector<MultiStreamChannel> configuredChannels;
	string primaryId;
	{
		lock_guard lock(mutex);
		if (active) {
			error = "Multistream is already active.";
			return false;
		}
		CollectRetiredDestinations();
		configuredChannels = channels;
		primaryId = primaryChannelId;
		sessionMainVideoEncoder = OBSEncoderAutoRelease{obs_encoder_get_ref(videoEncoder)};
	}

	portraitBitrateKbps = outputSettings.portraitBitrateKbps > 0 ? std::clamp(outputSettings.portraitBitrateKbps, 100, 100000) : 6000;
	/* The main output is already sending this one. */
	if (!primaryId.empty()) {
		configuredChannels.erase(
			remove_if(configuredChannels.begin(), configuredChannels.end(),
				  [&primaryId](const MultiStreamChannel &channel) { return channel.id == primaryId; }),
			configuredChannels.end());
	}

	vector<MultiStreamChannel> enabledChannels;
	for (const auto &channel : configuredChannels) {
		if (channel.enabled && !channel.server.empty() && !channel.streamKey.empty()) {
			enabledChannels.push_back(channel);
		}
	}
	/* Never silently substitute another mix for an explicitly selected track.
	 * The single-encoder overload is valid only for that encoder's own mix. */
	for (const auto &channel : enabledChannels) {
		obs_encoder_t *selected = channel.audioMixIndex < audioEncodersByTrack.size()
						  ? audioEncodersByTrack[channel.audioMixIndex]
						  : nullptr;
		if (!selected && obs_encoder_get_mixer_index(defaultAudioEncoder) == channel.audioMixIndex) {
			selected = defaultAudioEncoder;
		}
		if (!selected || obs_encoder_get_mixer_index(selected) != channel.audioMixIndex) {
			error = "Audio track " + to_string(channel.audioMixIndex + 1) + " is unavailable for " +
				channel.displayName + ". No substitute track was sent.";
			ReportFailure(enabledChannels, error);
			return false;
		}
		if (channel.vodTrackEnabled && GetStreamPlatformInfo(channel.platform).supportsVodTrack &&
		    channel.vodTrackIndex != channel.audioMixIndex &&
		    (channel.vodTrackIndex >= audioEncodersByTrack.size() ||
		     !audioEncodersByTrack[channel.vodTrackIndex] ||
		     obs_encoder_get_mixer_index(audioEncodersByTrack[channel.vodTrackIndex]) !=
			     channel.vodTrackIndex)) {
			error = "VOD audio track " + to_string(channel.vodTrackIndex + 1) + " is unavailable for " +
				channel.displayName + ".";
			ReportFailure(enabledChannels, error);
			return false;
		}
	}
	/* A portrait primary is prepared before the main OBS output starts. For
	 * additional destinations, create the shared portrait pipeline only when
	 * at least one of them is enabled; disabled vertical destinations are wired
	 * lazily by SetChannelEnabled(). */
	const auto portraitChannel =
		find_if(configuredChannels.cbegin(), configuredChannels.cend(), [](const MultiStreamChannel &channel) {
			return channel.enabled && channel.videoLayout == MultiStreamVideoLayout::Portrait &&
			       !channel.server.empty() && !channel.streamKey.empty();
		});
	if (portraitChannel != configuredChannels.cend() && !portraitVideoEncoder &&
	    !PreparePortraitPipeline(videoEncoder, portraitChannel->portraitFit, error)) {
		ReportFailure(enabledChannels, error);
		return false;
	}

	vector<shared_ptr<Destination>> prepared;
	auto abort = [&](string &target, string message) {
		for (const auto &destination : prepared) {
			DisconnectSignals(*destination);
		}
		prepared.clear();
		target = std::move(message);
		ReportFailure(enabledChannels, target);
		return false;
	};

	for (const auto &channel : configuredChannels) {
		if (channel.server.empty() || channel.streamKey.empty()) {
			continue;
		}

		OBSDataAutoRelease serviceSettings = obs_data_create();
		obs_data_set_string(serviceSettings, "server", channel.server.c_str());
		obs_data_set_string(serviceSettings, "key", channel.streamKey.c_str());

		auto destination = make_shared<Destination>();
		destination->owner = this;
		destination->channel = channel;
		destination->dynamicBitrateRequested = outputSettings.dynamicBitrate;

		const string serviceName = "multistream_service_" + channel.id;
		destination->service = obs_service_create("rtmp_custom", serviceName.c_str(), serviceSettings, nullptr);
		if (!destination->service) {
			return abort(error, "Could not create the RTMP service for " + channel.displayName + ".");
		}

		const string outputName = "multistream_output_" + channel.id;
		destination->output = obs_output_create("rtmp_output", outputName.c_str(), nullptr, nullptr);
		if (!destination->output) {
			return abort(error, "Could not create the RTMP output for " + channel.displayName + ".");
		}

		/* audioEncodersByTrack keeps one slot per OBS audio track, so the
		 * index the user selected always refers to the same track. */
		obs_encoder_t *selectedAudioEncoder = (channel.audioMixIndex < audioEncodersByTrack.size() &&
						       audioEncodersByTrack[channel.audioMixIndex])
							      ? audioEncodersByTrack[channel.audioMixIndex]
							      : defaultAudioEncoder;

		obs_encoder_t *selectedVideoEncoder = channel.videoLayout == MultiStreamVideoLayout::Portrait
							      ? portraitVideoEncoder.Get()
							      : videoEncoder;
		obs_output_set_video_encoder(destination->output, selectedVideoEncoder);
		obs_output_set_audio_encoder(destination->output, selectedAudioEncoder, 0);

		/* Only Twitch accepts a second audio track for the recorded VOD.
		 * Sending one to a platform that does not expect it is at best
		 * ignored and at worst refused, so the catalog gates it. */
		if (channel.vodTrackEnabled && GetStreamPlatformInfo(channel.platform).supportsVodTrack &&
		    channel.vodTrackIndex != channel.audioMixIndex &&
		    channel.vodTrackIndex < audioEncodersByTrack.size() &&
		    audioEncodersByTrack[channel.vodTrackIndex]) {
			obs_output_set_audio_encoder(destination->output, audioEncodersByTrack[channel.vodTrackIndex],
						     1);
		}
		obs_output_set_service(destination->output, destination->service);
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "bind_ip", outputSettings.bindIp.c_str());
		obs_data_set_string(settings, "ip_family", outputSettings.ipFamily.c_str());
		/* RTMP DBR updates the encoder itself, not only this connection. Main
		 * is borrowed, and Portrait can also serve the primary/record/replay.
		 * Even a single managed destination cannot prove exclusive ownership,
		 * including consumers attached later. Leave adaptation to the primary
		 * output; never create competing controllers or duplicate encoders. */
		obs_data_set_bool(settings, "dyn_bitrate", false);
		if (destination->dynamicBitrateRequested) {
			blog(LOG_INFO, "[multistream] Additional output '%s': dynamic bitrate disabled; "
				       "the primary output retains shared encoder authority.", channel.id.c_str());
		}
#ifdef _WIN32
		obs_data_set_bool(settings, "new_socket_loop_enabled", outputSettings.newSocketLoop);
		obs_data_set_bool(settings, "low_latency_mode_enabled", outputSettings.lowLatency);
#endif
		obs_output_update(destination->output, settings);
		obs_output_set_delay(destination->output, outputSettings.delaySeconds,
				     outputSettings.preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0);
		obs_output_set_reconnect_settings(destination->output, outputSettings.reconnect.maxRetries,
						  outputSettings.reconnect.retryDelaySeconds);
		destination->state = MultiStreamChannelState::Idle;
		ConnectSignals(*destination);
		prepared.emplace_back(std::move(destination));
	}

	vector<shared_ptr<Destination>> toStart;
	uint64_t generation;
	RetireDestinations();
	{
		lock_guard lock(mutex);
		destinations = std::move(prepared);
		sessionRunning = true;
		generation = ++sessionGeneration;
		active = true;
		for (const auto &destination : destinations) {
			if (destination->channel.enabled) {
				toStart.push_back(destination);
			}
		}
	}

	bool anyStarted = false;
	for (const auto &destination : toStart) {
		if (!CanStartDestination(destination, generation)) {
			continue;
		}
		UpdateState(*destination, MultiStreamChannelState::Starting);
		if (!CanStartDestination(destination, generation)) {
			continue;
		}
		if (obs_output_start(destination->output)) {
			anyStarted = true;
			continue;
		}

		if (!CanStartDestination(destination, generation)) {
			continue;
		}
		const char *lastError = obs_output_get_last_error(destination->output);
		UpdateState(*destination, MultiStreamChannelState::Failed,
			    lastError && *lastError ? lastError : "The output could not be started.");
	}
	{
		lock_guard lock(mutex);
		if (!sessionRunning || sessionGeneration != generation) {
			error = "Multistream start was cancelled.";
			return false;
		}
	}

	if (!anyStarted && !toStart.empty()) {
		{
			lock_guard lock(mutex);
			active = false;
		}
		error = "None of the configured multistream outputs could be started.";
		return false;
	}

	error.clear();
	return true;
}

void MultiStreamManager::Stop(bool force)
{
	vector<shared_ptr<Destination>> outputs;
	{
		lock_guard lock(mutex);
		outputs = destinations;
		sessionRunning = false;
		++sessionGeneration;
		for (const auto &destination : destinations) {
			destination->restartPending = false;
		}
		for (const auto &retired : retiredDestinations) {
			outputs.push_back(retired);
		}
	}

	/* The primary output's stop signal can call us from a libobs thread.
	 * Cancel immediately, then serialize output control with queued restarts.
	 * Capture this session's destinations so a late stop cannot stop a new one. */
	auto stopOutputs = [this, outputs = std::move(outputs), force]() {
		for (const auto &destination : outputs) {
			MultiStreamChannelState state;
			bool stopPending;
			{
				lock_guard lock(mutex);
				state = destination->state;
				stopPending = destination->stopPending;
			}
			/* The native primary's stop callback calls Stop() a second time.
			 * A secondary may already have signalled Idle while libobs still
			 * reports it active during encoder teardown. Stopping it again then
			 * would leave our state at Stopping with no further stop signal. */
			if (state == MultiStreamChannelState::Stopping || stopPending) {
				continue;
			}
			const bool outputActive = obs_output_active(destination->output);
			if (!outputActive && state != MultiStreamChannelState::Starting) {
				continue;
			}
			UpdateState(*destination, MultiStreamChannelState::Stopping);
			if (force || !outputActive) {
				obs_output_force_stop(destination->output);
			} else {
				obs_output_stop(destination->output);
			}
		}

		/* An output that never signalled "start" leaves no stop signal behind, so
		 * the active flag has to be recomputed here as well. Without this the
		 * manager stays permanently active and refuses every later Configure(). */
		lock_guard lock(mutex);
		CollectRetiredDestinations();
		RecomputeActiveLocked();
	};
	if (QThread::currentThread() == thread()) {
		stopOutputs();
	} else {
		QMetaObject::invokeMethod(this, std::move(stopOutputs), Qt::QueuedConnection);
	}
}

bool MultiStreamManager::SetChannelEnabled(const string &channelId, bool enabled, string &error)
{
	shared_ptr<Destination> destination;
	string channelIdToPersist;
	bool previousEnabled = false;
	MultiStreamVideoLayout layout = MultiStreamVideoLayout::Main;
	MultiStreamPortraitFit fit = MultiStreamPortraitFit::Fill;
	{
		lock_guard lock(mutex);
		auto channel = find_if(channels.begin(), channels.end(),
				       [&](const MultiStreamChannel &item) { return item.id == channelId; });
		if (channel == channels.end()) {
			error = "The selected multistream channel does not exist.";
			return false;
		}
		auto output = find_if(destinations.begin(), destinations.end(),
				      [&](const auto &item) { return item->channel.id == channelId; });
		if (output != destinations.end() && sessionRunning) {
			destination = *output;
		} else if (!destinations.empty() && enabled && active) {
			error = "This channel has no prepared streaming output.";
			return false;
		}
		previousEnabled = channel->enabled;
		layout = channel->videoLayout;
		fit = channel->portraitFit;
		channelIdToPersist = channelId;
	}
	if (enabled && destination) {
		obs_encoder_t *audio = obs_output_get_audio_encoder(destination->output, 0);
		if (!audio || obs_encoder_get_mixer_index(audio) != destination->channel.audioMixIndex) {
			error = "Audio track " + to_string(destination->channel.audioMixIndex + 1) +
				" is unavailable for " + destination->channel.displayName +
				". No substitute track was sent.";
			return false;
		}
		if (destination->channel.vodTrackEnabled &&
		    GetStreamPlatformInfo(destination->channel.platform).supportsVodTrack &&
		    destination->channel.vodTrackIndex != destination->channel.audioMixIndex) {
			obs_encoder_t *vod = obs_output_get_audio_encoder(destination->output, 1);
			if (!vod || obs_encoder_get_mixer_index(vod) != destination->channel.vodTrackIndex) {
				error = "The selected VOD audio track is unavailable for " +
					destination->channel.displayName + ".";
				return false;
			}
		}
	}

	if (enabled && destination && layout == MultiStreamVideoLayout::Portrait && !portraitVideoEncoder) {
		if (!sessionMainVideoEncoder || !PreparePortraitPipeline(sessionMainVideoEncoder, fit, error)) {
			return false;
		}
		lock_guard lock(mutex);
		for (const auto &item : destinations) {
			if (item->channel.videoLayout == MultiStreamVideoLayout::Portrait) {
				obs_output_set_video_encoder(item->output, portraitVideoEncoder);
			}
		}
	}

	{
		lock_guard lock(mutex);
		auto channel = find_if(channels.begin(), channels.end(),
				       [&](const MultiStreamChannel &item) { return item.id == channelId; });
		if (channel == channels.end()) {
			error = "The selected multistream channel no longer exists.";
			return false;
		}
		channel->enabled = enabled;
		if (destination) {
			destination->channel.enabled = enabled;
		}
	}

	if (!PersistEnabled(channelIdToPersist, enabled, error)) {
		lock_guard lock(mutex);
		auto channel = find_if(channels.begin(), channels.end(),
				       [&](const MultiStreamChannel &item) { return item.id == channelId; });
		if (channel != channels.end()) {
			channel->enabled = previousEnabled;
		}
		if (destination) {
			destination->channel.enabled = previousEnabled;
		}
		return false;
	}
	if (!destination) {
		error.clear();
		return true;
	}
	bool stopping;
	uint64_t generation;
	MultiStreamChannelState state;
	{
		lock_guard lock(mutex);
		stopping = destination->stopPending;
		generation = sessionGeneration;
		state = destination->state;
		/* A quick off/on must wait for the same stop as manual reconnect. */
		if (!enabled) {
			destination->restartPending = false;
		} else if (stopping) {
			destination->restartPending = true;
		}
		RecomputeActiveLocked();
	}

	if (enabled) {
		if (stopping) {
			QueueRestart(*destination);
			error.clear();
			return true;
		}
		if (obs_output_active(destination->output) || state == MultiStreamChannelState::Starting) {
			error.clear();
			return true;
		}
		UpdateState(*destination, MultiStreamChannelState::Starting);
		if (!CanStartDestination(destination, generation)) {
			error = "Starting this destination was cancelled.";
			return false;
		}
		if (!obs_output_start(destination->output)) {
			const char *lastError = obs_output_get_last_error(destination->output);
			UpdateState(*destination, MultiStreamChannelState::Failed,
				    lastError && *lastError ? lastError : "The output could not be started.");
			error = "Could not start " + destination->channel.displayName + ".";
			return false;
		}
	} else {
		if (stopping) {
			// The stop already in progress will finish with no restart.
		} else if (obs_output_active(destination->output) || state == MultiStreamChannelState::Starting) {
			const bool outputActive = obs_output_active(destination->output);
			UpdateState(*destination, MultiStreamChannelState::Stopping);
			if (outputActive) {
				obs_output_stop(destination->output);
			} else {
				obs_output_force_stop(destination->output);
			}
		} else {
			UpdateState(*destination, MultiStreamChannelState::Idle);
		}
	}

	error.clear();
	return true;
}

bool MultiStreamManager::RestartChannel(const string &channelId, string &error)
{
	shared_ptr<Destination> destination;
	bool mustStop = false;
	{
		lock_guard lock(mutex);
		auto item = find_if(destinations.begin(), destinations.end(),
				    [&](const auto &entry) { return entry->channel.id == channelId; });
		if (!sessionRunning || item == destinations.end() || !(*item)->channel.enabled) {
			error = "Only an enabled destination in the current streaming session can be reconnected.";
			return false;
		}
		destination = *item;
		if (!destination->restartPending) {
			destination->restartPending = true;
			mustStop = !destination->stopPending &&
				   (obs_output_active(destination->output) ||
				    destination->state == MultiStreamChannelState::Starting);
		}
		RecomputeActiveLocked();
	}
	if (mustStop) {
		UpdateState(*destination, MultiStreamChannelState::Stopping);
		obs_output_force_stop(destination->output);
	}
	QueueRestart(*destination);
	error.clear();
	return true;
}

void MultiStreamManager::QueueRestart(Destination &destination)
{
	weak_ptr<Destination> pending;
	{
		lock_guard lock(mutex);
		if (!sessionRunning || !destination.restartPending || destination.restartQueued) {
			return;
		}
		auto item = find_if(destinations.begin(), destinations.end(),
				    [&](const auto &entry) { return entry.get() == &destination; });
		if (item == destinations.end()) {
			return;
		}
		destination.restartQueued = true;
		pending = *item;
	}
	/* Never start from the stop signal: libobs may still be ending capture on
	 * another thread. QObject cancels this work when the manager is destroyed. */
	QTimer::singleShot(0, this, [this, pending]() { ResumeRestart(pending); });
}

void MultiStreamManager::ResumeRestart(const weak_ptr<Destination> &pending)
{
	auto destination = pending.lock();
	if (!destination) {
		return;
	}
	uint64_t generation;
	{
		lock_guard lock(mutex);
		if (!sessionRunning || !destination->restartPending || !destination->channel.enabled ||
		    find(destinations.begin(), destinations.end(), destination) == destinations.end()) {
			destination->restartQueued = false;
			return;
		}
		if (destination->state == MultiStreamChannelState::Stopping || obs_output_active(destination->output)) {
			QTimer::singleShot(25, this, [this, pending]() { ResumeRestart(pending); });
			return;
		}
		destination->restartPending = false;
		destination->restartQueued = false;
		destination->stopPending = false;
		generation = sessionGeneration;
		++destination->reconnects;
	}
	UpdateState(*destination, MultiStreamChannelState::Starting);
	if (!CanStartDestination(destination, generation)) {
		return;
	}
	if (!obs_output_start(destination->output)) {
		const char *error = obs_output_get_last_error(destination->output);
		UpdateState(*destination, MultiStreamChannelState::Failed,
			    error && *error ? error : "The output could not be restarted.");
	}
}

bool MultiStreamManager::CanStartDestination(const shared_ptr<Destination> &destination, uint64_t generation) const
{
	lock_guard lock(mutex);
	return sessionRunning && sessionGeneration == generation && destination->channel.enabled &&
	       !destination->stopPending && find(destinations.begin(), destinations.end(), destination) != destinations.end();
}

void MultiStreamManager::SetPrimaryChannelId(const string &channelId)
{
	lock_guard lock(mutex);
	primaryChannelId = channelId;
}

string MultiStreamManager::PrimaryChannelId() const
{
	lock_guard lock(mutex);
	return primaryChannelId;
}

MultiStreamChannel MultiStreamManager::FirstReadyChannel() const
{
	lock_guard lock(mutex);
	return MultistreamChannelPlan::FirstReady(channels);
}

obs_encoder_t *MultiStreamManager::PreparePrimaryVideoEncoder(obs_encoder_t *mainVideoEncoder, string &error, int bitrate)
{
	if (!mainVideoEncoder) {
		error = "The main video encoder is not ready.";
		return nullptr;
	}

	portraitBitrateKbps = bitrate > 0 ? std::clamp(bitrate, 100, 100000) : 6000;
	MultiStreamChannel primary;
	{
		lock_guard lock(mutex);
		auto item = find_if(channels.cbegin(), channels.cend(), [this](const MultiStreamChannel &channel) {
			return channel.id == primaryChannelId;
		});
		if (item != channels.cend()) {
			primary = *item;
		}
	}

	if (primary.id.empty() || primary.videoLayout != MultiStreamVideoLayout::Portrait) {
		DestroyPortraitPipeline();
		error.clear();
		return mainVideoEncoder;
	}

	if (!PreparePortraitPipeline(mainVideoEncoder, primary.portraitFit, error)) {
		return nullptr;
	}
	return portraitVideoEncoder;
}

void MultiStreamManager::OnMainChannelChanged(void *data, calldata_t *params)
{
	if (calldata_int(params, "channel") != 0) {
		return;
	}
	auto *manager = static_cast<MultiStreamManager *>(data);
	lock_guard lock(manager->mutex);
	if (!manager->portraitScene) {
		return;
	}
	manager->SetPortraitSource(static_cast<obs_source_t *>(calldata_ptr(params, "source")), manager->portraitFit);
}

bool MultiStreamManager::PreparePortraitPipeline(obs_encoder_t *mainVideoEncoder, MultiStreamPortraitFit fit,
						 std::string &error)
{
	DestroyPortraitPipeline();

	obs_video_info videoInfo{};
	if (!obs_get_video_info(&videoInfo)) {
		error = "The OBS video pipeline is not ready for the vertical output.";
		return false;
	}
	videoInfo.base_width = PORTRAIT_WIDTH;
	videoInfo.base_height = PORTRAIT_HEIGHT;
	videoInfo.output_width = PORTRAIT_WIDTH;
	videoInfo.output_height = PORTRAIT_HEIGHT;
	videoInfo.scale_type = OBS_SCALE_BICUBIC;

	OBSCanvas sharedCanvas;
	{
		lock_guard providerLock(PortraitProviderMutex());
		if (PortraitProvider()) {
			sharedCanvas = PortraitProvider()();
			if (!sharedCanvas) {
				error = "Enable Multi Canvas before starting a portrait destination.";
				return false;
			}
		}
	}
	if (sharedCanvas) {
		/* Dual-canvas mode: the vertical canvas already has its own scenes. */
		portraitCanvas = obs_canvas_get_ref(sharedCanvas);
		portraitCanvasShared = true;
		blog(LOG_INFO, "Multistream: portrait output uses the scene collection's vertical canvas '%s'",
		     obs_canvas_get_name(sharedCanvas));
	} else {
		portraitCanvasShared = false;
		portraitCanvas = obs_canvas_create_private("multistream_portrait_canvas", &videoInfo,
							   ACTIVATE | SCENE_REF | EPHEMERAL);
		if (!portraitCanvas) {
			error = "Could not create the 9:16 video canvas.";
			return false;
		}
		portraitScene = obs_canvas_scene_create(portraitCanvas, "multistream_portrait_program");
		if (!portraitScene) {
			error = "Could not create the vertical program scene.";
			DestroyPortraitPipeline();
			return false;
		}
		portraitFit = fit;
		OBSSourceAutoRelease program = obs_get_output_source(0);
		SetPortraitSource(program, fit);
		obs_canvas_set_channel(portraitCanvas, 0, obs_scene_get_source(portraitScene));
	}

	const char *mainEncoderId = obs_encoder_get_id(mainVideoEncoder);
	const char *mainCodec = obs_encoder_get_codec(mainVideoEncoder);
	const bool mainIsH264 = mainCodec && string(mainCodec) == "h264";
	const char *encoderId = mainEncoderId;
	if (!mainIsH264) {
		/* Vertical RTMP destinations are overwhelmingly H.264-only. Preserve
		 * hardware acceleration when the primary encoder has an H.264 sibling,
		 * otherwise fall back to the encoder OBS always ships. */
		const string id = mainEncoderId ? mainEncoderId : "";
		const char *candidates[3] = {nullptr, nullptr, "obs_x264"};
		if (id.find("nvenc") != string::npos) {
			candidates[0] = "obs_nvenc_h264_tex";
			candidates[1] = "ffmpeg_nvenc";
		} else if (id.find("qsv") != string::npos) {
			candidates[0] = "obs_qsv11_v2";
			candidates[1] = "obs_qsv11";
		} else if (id.find("amf") != string::npos) {
			candidates[0] = "h264_texture_amf";
		} else if (id.find("videotoolbox") != string::npos || id.find("apple") != string::npos) {
			candidates[0] = "com.apple.videotoolbox.videoencoder.ave.avc";
		}
		for (const char *candidate : candidates) {
			if (candidate && obs_get_encoder_codec(candidate)) {
				encoderId = candidate;
				break;
			}
		}
		blog(LOG_INFO, "Multistream: portrait output uses H.264 encoder '%s' instead of '%s'", encoderId,
		     mainEncoderId ? mainEncoderId : "unknown");
	}

	OBSDataAutoRelease encoderSettings = obs_data_create();
	if (mainIsH264) {
		OBSDataAutoRelease originalSettings = obs_encoder_get_settings(mainVideoEncoder);
		obs_data_apply(encoderSettings, originalSettings);
	}
	obs_data_set_string(encoderSettings, "rate_control", "CBR");
	obs_data_set_int(encoderSettings, "bitrate", portraitBitrateKbps);
	obs_data_set_int(encoderSettings, "keyint_sec", 2);

	portraitVideoEncoder =
		obs_video_encoder_create(encoderId, "multistream_portrait_video", encoderSettings, nullptr);
	if (!portraitVideoEncoder) {
		error = "Could not create a second video encoder for the vertical output.";
		DestroyPortraitPipeline();
		return false;
	}
	obs_encoder_set_video(portraitVideoEncoder, obs_canvas_get_video(portraitCanvas));

	if (!portraitCanvasShared) {
		signal_handler_connect(obs_get_signal_handler(), "channel_change", OnMainChannelChanged, this);
		mainChannelSignalConnected = true;
	}
	error.clear();
	return true;
}

void MultiStreamManager::SetPortraitSource(obs_source_t *source, MultiStreamPortraitFit fit)
{
	if (portraitSceneItem) {
		obs_sceneitem_remove(portraitSceneItem);
		portraitSceneItem = nullptr;
	}
	if (!portraitScene || !source) {
		return;
	}

	portraitSceneItem = obs_scene_add(portraitScene, source);
	if (!portraitSceneItem) {
		return;
	}

	obs_sceneitem_set_alignment(portraitSceneItem, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_alignment(portraitSceneItem, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_type(portraitSceneItem, fit == MultiStreamPortraitFit::Fill ? OBS_BOUNDS_SCALE_OUTER
											     : OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_scale_filter(portraitSceneItem, OBS_SCALE_LANCZOS);
	const vec2 bounds{static_cast<float>(PORTRAIT_WIDTH), static_cast<float>(PORTRAIT_HEIGHT)};
	const vec2 position{PORTRAIT_WIDTH / 2.0f, PORTRAIT_HEIGHT / 2.0f};
	obs_sceneitem_set_bounds(portraitSceneItem, &bounds);
	obs_sceneitem_set_pos(portraitSceneItem, &position);
}

void MultiStreamManager::DestroyPortraitPipeline()
{
	if (mainChannelSignalConnected) {
		signal_handler_disconnect(obs_get_signal_handler(), "channel_change", OnMainChannelChanged, this);
		mainChannelSignalConnected = false;
	}
	portraitVideoEncoder = nullptr;
	if (portraitCanvas && !portraitCanvasShared) {
		obs_canvas_set_channel(portraitCanvas, 0, nullptr);
	}
	portraitCanvasShared = false;
	portraitSceneItem = nullptr;
	portraitScene = nullptr;
	portraitCanvas = nullptr;
}

std::mutex &MultiStreamManager::PortraitProviderMutex()
{
	static std::mutex providerMutex;
	return providerMutex;
}

MultiStreamManager::PortraitCanvasProvider &MultiStreamManager::PortraitProvider()
{
	static PortraitCanvasProvider provider;
	return provider;
}

void MultiStreamManager::SetPortraitCanvasProvider(PortraitCanvasProvider provider)
{
	lock_guard lock(PortraitProviderMutex());
	PortraitProvider() = std::move(provider);
}

std::vector<MultiStreamManager::EncoderGroup> MultiStreamManager::EncoderGroups() const
{
	lock_guard lock(mutex);
	EncoderGroup mainGroup;
	mainGroup.layout = MultiStreamVideoLayout::Main;
	EncoderGroup portraitGroup;
	portraitGroup.layout = MultiStreamVideoLayout::Portrait;
	if (sessionMainVideoEncoder) {
		mainGroup.encoderId = obs_encoder_get_id(sessionMainVideoEncoder);
	}
	if (portraitVideoEncoder) {
		portraitGroup.encoderId = obs_encoder_get_id(portraitVideoEncoder);
	}
	for (const auto &channel : channels) {
		if (!channel.enabled) {
			continue;
		}
		auto &group = channel.videoLayout == MultiStreamVideoLayout::Portrait ? portraitGroup : mainGroup;
		group.channelIds.push_back(channel.id);
	}
	std::vector<EncoderGroup> groups;
	if (!mainGroup.channelIds.empty()) {
		groups.push_back(std::move(mainGroup));
	}
	if (!portraitGroup.channelIds.empty()) {
		groups.push_back(std::move(portraitGroup));
	}
	return groups;
}

bool MultiStreamManager::IsActive() const
{
	lock_guard lock(mutex);
	return active;
}

bool MultiStreamManager::HasAdditionalChannels() const
{
	lock_guard lock(mutex);
	/* Prepare disabled destinations too, allowing them to be switched on while
	 * the primary output is already live. */
	return any_of(channels.begin(), channels.end(), [this](const MultiStreamChannel &channel) {
		return !channel.server.empty() && !channel.streamKey.empty() && channel.id != primaryChannelId;
	});
}

vector<MultiStreamChannel> MultiStreamManager::ConfiguredChannels() const
{
	lock_guard lock(mutex);
	return channels;
}

vector<MultiStreamChannelSnapshot> MultiStreamManager::Snapshot() const
{
	lock_guard lock(mutex);
	vector<MultiStreamChannelSnapshot> result;
	result.reserve(destinations.size());
	for (const auto &destination : destinations) {
		result.push_back({destination->channel.id, destination->channel.displayName, destination->state,
				  destination->lastError, ReadHealth(*destination), destination->dynamicBitrateRequested,
				  false, destination->dynamicBitrateRequested
						 ? MultiStreamDynamicBitrateReason::SharedEncoderAuthority
						 : MultiStreamDynamicBitrateReason::NotRequested});
	}
	return result;
}

vector<MultiStreamManager::ChannelOutput> MultiStreamManager::ChannelOutputs() const
{
	lock_guard lock(mutex);
	vector<ChannelOutput> result;
	result.reserve(channels.size());
	/* Driven by the configured list rather than by the live destinations, so
	 * the table shows every enabled channel even before anything is sending.
	 * A destination only exists while the output is up. */
	for (const auto &channel : channels) {
		if (!channel.enabled || channel.id == primaryChannelId) {
			continue;
		}

		ChannelOutput entry{channel.id, channel.displayName, {}};
		for (const auto &destination : destinations) {
			if (destination->channel.id == channel.id && destination->output) {
				entry.output = OBSOutputAutoRelease{obs_output_get_ref(destination->output)};
				break;
			}
		}
		result.emplace_back(std::move(entry));
	}
	return result;
}

void MultiStreamManager::SetStateCallback(StateCallback callback)
{
	lock_guard lock(mutex);
	stateCallback = std::move(callback);
}

bool MultiStreamManager::PersistEnabled(const string &channelId, bool enabled, string &error)
{
	if (channelId.empty()) {
		error.clear();
		return true;
	}
	return MultistreamChannelStore::SetEnabled(channelId, enabled, error);
}

void MultiStreamManager::ReportFailure(const vector<MultiStreamChannel> &failedChannels, const string &error)
{
	StateCallback callback;
	{
		lock_guard lock(mutex);
		callback = stateCallback;
	}
	if (!callback) {
		return;
	}

	for (const auto &channel : failedChannels) {
		callback({channel.id, channel.displayName, MultiStreamChannelState::Failed, error});
	}
}

void MultiStreamManager::RetireDestinations()
{
	vector<shared_ptr<Destination>> retired;
	{
		lock_guard lock(mutex);
		retired = std::move(destinations);
		++sessionGeneration;
		for (const auto &destination : retired) {
			retiredDestinations.push_back(destination);
			destination->restartPending = false;
		}
	}
	/* Disconnect may wait for an in-flight signal whose callback needs mutex. */
	for (const auto &destination : retired) {
		DisconnectSignals(*destination);
	}
}

void MultiStreamManager::CollectRetiredDestinations()
{
	retiredDestinations.erase(remove_if(retiredDestinations.begin(), retiredDestinations.end(),
					    [](const shared_ptr<Destination> &destination) {
						    return !obs_output_active(destination->output);
					    }),
				  retiredDestinations.end());
}

bool MultiStreamManager::RecomputeActiveLocked()
{
	active = false;
	for (const auto &destination : destinations) {
		if (destination->restartPending || destination->state == MultiStreamChannelState::Starting ||
		    destination->state == MultiStreamChannelState::Live ||
		    destination->state == MultiStreamChannelState::Stopping) {
			active = true;
			break;
		}
	}
	return active;
}

MultiStreamChannelHealth MultiStreamManager::ReadHealth(const Destination &destination)
{
	MultiStreamChannelHealth health;
	if (!destination.output || !obs_output_active(destination.output)) {
		return health;
	}

	health.congestion = obs_output_get_congestion(destination.output);
	health.droppedFrames = obs_output_get_frames_dropped(destination.output);
	health.totalFrames = obs_output_get_total_frames(destination.output);
	health.totalBytes = static_cast<uint64_t>(obs_output_get_total_bytes(destination.output));
	health.reconnects = destination.reconnects;
	if (destination.liveSinceUnixTime > 0) {
		const int64_t now =
			chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
		health.liveSeconds = max<int64_t>(0, now - destination.liveSinceUnixTime);
	}
	return health;
}

void MultiStreamManager::OnOutputStart(void *data, calldata_t *)
{
	auto *destination = static_cast<Destination *>(data);
	destination->owner->UpdateState(*destination, MultiStreamChannelState::Live);
}

void MultiStreamManager::OnOutputReconnect(void *data, calldata_t *)
{
	auto *destination = static_cast<Destination *>(data);
	/* Reconnecting is not idle and not live: report it as starting so the
	 * card stops claiming the destination is fine. */
	destination->owner->UpdateState(*destination, MultiStreamChannelState::Starting, nullptr, true);
}

void MultiStreamManager::OnOutputStopping(void *data, calldata_t *)
{
	auto *destination = static_cast<Destination *>(data);
	destination->owner->UpdateState(*destination, MultiStreamChannelState::Stopping);
}

void MultiStreamManager::OnOutputStop(void *data, calldata_t *params)
{
	auto *destination = static_cast<Destination *>(data);
	const int code = static_cast<int>(calldata_int(params, "code"));
	const char *lastError = calldata_string(params, "last_error");
	const auto state = code == OBS_OUTPUT_SUCCESS ? MultiStreamChannelState::Idle : MultiStreamChannelState::Failed;
	destination->owner->UpdateState(*destination, state, lastError);
	destination->owner->QueueRestart(*destination);
}

void MultiStreamManager::ConnectSignals(Destination &destination)
{
	signal_handler_t *handler = obs_output_get_signal_handler(destination.output);
	if (!handler) {
		return;
	}
	signal_handler_connect(handler, "start", OnOutputStart, &destination);
	signal_handler_connect(handler, "reconnect_success", OnOutputStart, &destination);
	signal_handler_connect(handler, "stopping", OnOutputStopping, &destination);
	signal_handler_connect(handler, "stop", OnOutputStop, &destination);
	signal_handler_connect(handler, "reconnect", OnOutputReconnect, &destination);
	destination.signalsConnected = true;
}

void MultiStreamManager::DisconnectSignals(Destination &destination)
{
	if (!destination.signalsConnected) {
		return;
	}
	signal_handler_t *handler = obs_output_get_signal_handler(destination.output);
	if (!handler) {
		return;
	}
	signal_handler_disconnect(handler, "start", OnOutputStart, &destination);
	signal_handler_disconnect(handler, "reconnect_success", OnOutputStart, &destination);
	signal_handler_disconnect(handler, "stopping", OnOutputStopping, &destination);
	signal_handler_disconnect(handler, "stop", OnOutputStop, &destination);
	signal_handler_disconnect(handler, "reconnect", OnOutputReconnect, &destination);
	destination.signalsConnected = false;
}

void MultiStreamManager::UpdateState(Destination &destination, MultiStreamChannelState state, const char *lastError,
				     bool countReconnect)
{
	StateCallback callback;
	MultiStreamChannelSnapshot snapshot;
	{
		lock_guard lock(mutex);
		/* Disconnected/retired outputs and late libobs start/reconnect signals
		 * cannot publish a new Live/Starting card after cancellation. */
		if (find_if(destinations.begin(), destinations.end(), [&](const auto &item) {
			    return item.get() == &destination;
		    }) == destinations.end() ||
		    ((state == MultiStreamChannelState::Starting || state == MultiStreamChannelState::Live) &&
		     (!sessionRunning || !destination.channel.enabled || destination.stopPending))) {
			return;
		}
		if (countReconnect) {
			++destination.reconnects;
		}
		destination.state = state;
		if (state == MultiStreamChannelState::Stopping) {
			destination.stopPending = true;
		} else if (state == MultiStreamChannelState::Starting || state == MultiStreamChannelState::Live) {
			destination.stopPending = false;
		}
		destination.lastError = lastError ? lastError : "";
		if (state == MultiStreamChannelState::Live) {
			destination.liveSinceUnixTime =
				chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch())
					.count();
		} else {
			destination.liveSinceUnixTime = 0;
		}
		snapshot = {destination.channel.id, destination.channel.displayName, destination.state,
			    destination.lastError, ReadHealth(destination), destination.dynamicBitrateRequested, false,
			    destination.dynamicBitrateRequested ? MultiStreamDynamicBitrateReason::SharedEncoderAuthority
						       : MultiStreamDynamicBitrateReason::NotRequested};
		callback = stateCallback;
		RecomputeActiveLocked();
	}

	if (callback) {
		callback(snapshot);
	}
}
