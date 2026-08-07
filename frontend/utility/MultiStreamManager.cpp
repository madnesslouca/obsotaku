/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultiStreamManager.hpp"
#include "MultistreamChannelStore.hpp"

#include <util/base.h>

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

using namespace std;

namespace {
void WipeSecret(string &value)
{
	volatile char *data = value.empty() ? nullptr : value.data();
	for (size_t index = 0; index < value.size(); ++index)
		data[index] = 0;
	value.clear();
}

void WipeChannelSecrets(vector<MultiStreamChannel> &channels)
{
	for (auto &channel : channels)
		WipeSecret(channel.streamKey);
}
} // namespace

MultiStreamManager::~MultiStreamManager()
{
	vector<shared_ptr<Destination>> oldDestinations;
	{
		lock_guard lock(mutex);
		stateCallback = nullptr;
		oldDestinations = std::move(destinations);
		for (auto &retired : retiredDestinations)
			oldDestinations.emplace_back(std::move(retired));
		retiredDestinations.clear();
		active = false;
		WipeChannelSecrets(channels);
	}

	for (const auto &destination : oldDestinations) {
		DisconnectSignals(*destination);
		if (obs_output_active(destination->output))
			obs_output_force_stop(destination->output);
		WipeSecret(destination->channel.streamKey);
	}
}

bool MultiStreamManager::Configure(vector<MultiStreamChannel> newChannels, string &error)
{
	lock_guard lock(mutex);
	if (active) {
		error = "Multistream channels cannot be changed while an output is active.";
		return false;
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

	RetireDestinations();
	CollectRetiredDestinations();
	WipeChannelSecrets(channels);
	channels = std::move(newChannels);
	error.clear();
	return true;
}

bool MultiStreamManager::Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
			       const MultiStreamReconnectSettings &reconnect, string &error)
{
	return Start(videoEncoder, defaultAudioEncoder, {}, reconnect, error);
}

bool MultiStreamManager::Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
			       const vector<obs_encoder_t *> &audioEncodersByTrack,
			       const MultiStreamReconnectSettings &reconnect, string &error)
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
	}

	/* The main output is already sending this one. */
	if (!primaryId.empty()) {
		configuredChannels.erase(remove_if(configuredChannels.begin(), configuredChannels.end(),
						   [&primaryId](const MultiStreamChannel &channel) {
							   return channel.id == primaryId;
						   }),
					 configuredChannels.end());
	}

	vector<MultiStreamChannel> enabledChannels;
	for (const auto &channel : configuredChannels) {
		if (channel.enabled && !channel.server.empty() && !channel.streamKey.empty())
			enabledChannels.push_back(channel);
	}
	if (enabledChannels.empty()) {
		error = "Enable at least one multistream channel before going live.";
		return false;
	}

	vector<shared_ptr<Destination>> prepared;
	auto abort = [&](string &target, string message) {
		for (const auto &destination : prepared)
			DisconnectSignals(*destination);
		prepared.clear();
		target = std::move(message);
		ReportFailure(enabledChannels, target);
		return false;
	};

	for (const auto &channel : configuredChannels) {
		if (channel.server.empty() || channel.streamKey.empty())
			continue;

		OBSDataAutoRelease serviceSettings = obs_data_create();
		obs_data_set_string(serviceSettings, "server", channel.server.c_str());
		obs_data_set_string(serviceSettings, "key", channel.streamKey.c_str());

		auto destination = make_shared<Destination>();
		destination->owner = this;
		destination->channel = channel;

		const string serviceName = "multistream_service_" + channel.id;
		destination->service = obs_service_create("rtmp_custom", serviceName.c_str(), serviceSettings, nullptr);
		if (!destination->service)
			return abort(error, "Could not create the RTMP service for " + channel.displayName + ".");

		const string outputName = "multistream_output_" + channel.id;
		destination->output = obs_output_create("rtmp_output", outputName.c_str(), nullptr, nullptr);
		if (!destination->output)
			return abort(error, "Could not create the RTMP output for " + channel.displayName + ".");

		/* audioEncodersByTrack keeps one slot per OBS audio track, so the
		 * index the user selected always refers to the same track. */
		obs_encoder_t *selectedAudioEncoder =
			(channel.audioMixIndex < audioEncodersByTrack.size() &&
			 audioEncodersByTrack[channel.audioMixIndex])
				? audioEncodersByTrack[channel.audioMixIndex]
				: defaultAudioEncoder;

		obs_output_set_video_encoder(destination->output, videoEncoder);
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
		obs_output_set_reconnect_settings(destination->output, reconnect.maxRetries,
						  reconnect.retryDelaySeconds);
		destination->state = MultiStreamChannelState::Idle;
		ConnectSignals(*destination);
		prepared.emplace_back(std::move(destination));
	}

	vector<Destination *> toStart;
	{
		lock_guard lock(mutex);
		RetireDestinations();
		destinations = std::move(prepared);
		active = true;
		for (const auto &destination : destinations) {
			if (destination->channel.enabled)
				toStart.push_back(destination.get());
		}
	}

	bool anyStarted = false;
	for (Destination *destination : toStart) {
		UpdateState(*destination, MultiStreamChannelState::Starting);
		if (obs_output_start(destination->output)) {
			anyStarted = true;
			continue;
		}

		const char *lastError = obs_output_get_last_error(destination->output);
		UpdateState(*destination, MultiStreamChannelState::Failed,
			    lastError && *lastError ? lastError : "The output could not be started.");
	}

	if (!anyStarted) {
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
		for (const auto &retired : retiredDestinations)
			outputs.push_back(retired);
	}

	for (const auto &destination : outputs) {
		if (!obs_output_active(destination->output))
			continue;
		UpdateState(*destination, MultiStreamChannelState::Stopping);
		if (force)
			obs_output_force_stop(destination->output);
		else
			obs_output_stop(destination->output);
	}

	/* An output that never signalled "start" leaves no stop signal behind, so
	 * the active flag has to be recomputed here as well. Without this the
	 * manager stays permanently active and refuses every later Configure(). */
	lock_guard lock(mutex);
	CollectRetiredDestinations();
	RecomputeActiveLocked();
}

bool MultiStreamManager::SetChannelEnabled(const string &channelId, bool enabled, string &error)
{
	shared_ptr<Destination> destination;
	string channelIdToPersist;
	{
		lock_guard lock(mutex);
		auto channel = find_if(channels.begin(), channels.end(),
				       [&](const MultiStreamChannel &item) { return item.id == channelId; });
		if (channel == channels.end()) {
			error = "The selected multistream channel does not exist.";
			return false;
		}
		channel->enabled = enabled;
		channelIdToPersist = channelId;

		auto output = find_if(destinations.begin(), destinations.end(),
				      [&](const auto &item) { return item->channel.id == channelId; });
		if (output != destinations.end()) {
			destination = *output;
			destination->channel.enabled = enabled;
		} else if (!destinations.empty() && enabled) {
			error = "This channel has no prepared streaming output.";
			return false;
		}
	}

	PersistEnabled(channelIdToPersist, enabled);
	if (!destination) {
		error.clear();
		return true;
	}

	if (enabled) {
		if (obs_output_active(destination->output)) {
			error.clear();
			return true;
		}
		UpdateState(*destination, MultiStreamChannelState::Starting);
		if (!obs_output_start(destination->output)) {
			const char *lastError = obs_output_get_last_error(destination->output);
			UpdateState(*destination, MultiStreamChannelState::Failed,
				    lastError && *lastError ? lastError : "The output could not be started.");
			error = "Could not start " + destination->channel.displayName + ".";
			return false;
		}
	} else {
		if (obs_output_active(destination->output)) {
			UpdateState(*destination, MultiStreamChannelState::Stopping);
			obs_output_stop(destination->output);
		} else {
			UpdateState(*destination, MultiStreamChannelState::Idle);
		}
	}

	error.clear();
	return true;
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
	const auto ready = find_if(channels.begin(), channels.end(), [](const MultiStreamChannel &channel) {
		return channel.enabled && !channel.server.empty() && !channel.streamKey.empty();
	});
	return ready != channels.end() ? *ready : MultiStreamChannel{};
}

bool MultiStreamManager::IsActive() const
{
	lock_guard lock(mutex);
	return active;
}

bool MultiStreamManager::HasEnabledChannels() const
{
	lock_guard lock(mutex);
	/* The primary channel does not count: it travels on the main output, so a
	 * setup with only that one has nothing extra to fan out. */
	return any_of(channels.begin(), channels.end(), [this](const MultiStreamChannel &channel) {
		return channel.enabled && !channel.server.empty() && !channel.streamKey.empty() &&
		       channel.id != primaryChannelId;
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
				  destination->lastError, ReadHealth(*destination)});
	}
	return result;
}

void MultiStreamManager::SetStateCallback(StateCallback callback)
{
	lock_guard lock(mutex);
	stateCallback = std::move(callback);
}

void MultiStreamManager::PersistEnabled(const string &channelId, bool enabled)
{
	if (channelId.empty())
		return;
	string error;
	if (!MultistreamChannelStore::SetEnabled(channelId, enabled, error))
		blog(LOG_WARNING, "Could not store the multistream channel state: %s", error.c_str());
}

void MultiStreamManager::ReportFailure(const vector<MultiStreamChannel> &failedChannels, const string &error)
{
	StateCallback callback;
	{
		lock_guard lock(mutex);
		callback = stateCallback;
	}
	if (!callback)
		return;

	for (const auto &channel : failedChannels)
		callback({channel.id, channel.displayName, MultiStreamChannelState::Failed, error});
}

void MultiStreamManager::RetireDestinations()
{
	for (auto &destination : destinations) {
		DisconnectSignals(*destination);
		retiredDestinations.emplace_back(std::move(destination));
	}
	destinations.clear();
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
		if (destination->state == MultiStreamChannelState::Starting ||
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
	if (!destination.output || !obs_output_active(destination.output))
		return health;

	health.congestion = obs_output_get_congestion(destination.output);
	health.droppedFrames = obs_output_get_frames_dropped(destination.output);
	health.totalFrames = obs_output_get_total_frames(destination.output);
	health.totalBytes = static_cast<uint64_t>(obs_output_get_total_bytes(destination.output));
	health.reconnects = destination.reconnects;
	if (destination.liveSinceUnixTime > 0) {
		const int64_t now = chrono::duration_cast<chrono::seconds>(
					    chrono::system_clock::now().time_since_epoch())
					    .count();
		health.liveSeconds = max<int64_t>(0, now - destination.liveSinceUnixTime);
	}
	return health;
}

void MultiStreamManager::OnOutputStart(void *data, calldata_t *)
{
	auto *destination = static_cast<Destination *>(data);
	destination->liveSinceUnixTime =
		chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
	destination->owner->UpdateState(*destination, MultiStreamChannelState::Live);
}

void MultiStreamManager::OnOutputReconnect(void *data, calldata_t *)
{
	auto *destination = static_cast<Destination *>(data);
	++destination->reconnects;
	/* Reconnecting is not idle and not live: report it as starting so the
	 * card stops claiming the destination is fine. */
	destination->owner->UpdateState(*destination, MultiStreamChannelState::Starting);
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
}

void MultiStreamManager::ConnectSignals(Destination &destination)
{
	signal_handler_t *handler = obs_output_get_signal_handler(destination.output);
	if (!handler)
		return;
	signal_handler_connect(handler, "start", OnOutputStart, &destination);
	signal_handler_connect(handler, "stopping", OnOutputStopping, &destination);
	signal_handler_connect(handler, "stop", OnOutputStop, &destination);
	signal_handler_connect(handler, "reconnect", OnOutputReconnect, &destination);
	destination.signalsConnected = true;
}

void MultiStreamManager::DisconnectSignals(Destination &destination)
{
	if (!destination.signalsConnected)
		return;
	signal_handler_t *handler = obs_output_get_signal_handler(destination.output);
	if (!handler)
		return;
	signal_handler_disconnect(handler, "start", OnOutputStart, &destination);
	signal_handler_disconnect(handler, "stopping", OnOutputStopping, &destination);
	signal_handler_disconnect(handler, "stop", OnOutputStop, &destination);
	signal_handler_disconnect(handler, "reconnect", OnOutputReconnect, &destination);
	destination.signalsConnected = false;
}

void MultiStreamManager::UpdateState(Destination &destination, MultiStreamChannelState state, const char *lastError)
{
	StateCallback callback;
	MultiStreamChannelSnapshot snapshot;
	{
		lock_guard lock(mutex);
		destination.state = state;
		destination.lastError = lastError ? lastError : "";
		if (state != MultiStreamChannelState::Live)
			destination.liveSinceUnixTime = 0;
		snapshot = {destination.channel.id, destination.channel.displayName, destination.state,
			    destination.lastError, ReadHealth(destination)};
		callback = stateCallback;
		RecomputeActiveLocked();
	}

	if (callback)
		callback(snapshot);
}
