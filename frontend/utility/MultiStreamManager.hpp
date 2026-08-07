/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "StreamPlatform.hpp"

#include <obs.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class MultiStreamChannelState {
	Idle,
	Starting,
	Live,
	Stopping,
	Failed,
};

struct MultiStreamChannel {
	std::string id;
	std::string displayName;
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	std::string accountId;
	std::string server;
	std::string streamKey;
	/* Remote profile picture URL; the channel bar caches it on disk. */
	std::string avatarUrl;
	/* Broadcast metadata. An empty title means the channel follows the shared
	 * one from the stream info panel. */
	std::string title;
	std::string categoryId;
	std::string categoryName;
	/* Zero-based OBS audio track. Track 1 in the user interface is index 0. */
	size_t audioMixIndex = 0;
	bool vodTrackEnabled = false;
	size_t vodTrackIndex = 1;
	bool enabled = true;
};

/* Live delivery health for one destination. Only meaningful while the output
 * is active; everything is zero otherwise. */
struct MultiStreamChannelHealth {
	/* 0.0 = keeping up, 1.0 = the send buffer is full. */
	float congestion = 0.0f;
	int droppedFrames = 0;
	int totalFrames = 0;
	uint64_t totalBytes = 0;
	int64_t liveSeconds = 0;
	int reconnects = 0;
};

struct MultiStreamChannelSnapshot {
	std::string id;
	std::string displayName;
	MultiStreamChannelState state = MultiStreamChannelState::Idle;
	std::string lastError;
	MultiStreamChannelHealth health;
};

struct MultiStreamReconnectSettings {
	int maxRetries = 20;
	int retryDelaySeconds = 2;
};

class MultiStreamManager {
public:
	using StateCallback = std::function<void(const MultiStreamChannelSnapshot &)>;

	MultiStreamManager() = default;
	~MultiStreamManager();

	MultiStreamManager(const MultiStreamManager &) = delete;
	MultiStreamManager &operator=(const MultiStreamManager &) = delete;

	bool Configure(std::vector<MultiStreamChannel> channels, std::string &error);

	/* The channel already being sent by the application's main output. It is
	 * skipped here, otherwise the same destination would receive two uploads. */
	void SetPrimaryChannelId(const std::string &channelId);
	std::string PrimaryChannelId() const;

	/* First enabled channel with usable credentials, or an empty channel when
	 * there is none. Used to fill the main output when it has no service. */
	MultiStreamChannel FirstReadyChannel() const;

	/* audioEncodersByTrack is indexed by OBS audio track, so entries may be
	 * null. Compacting it would silently remap the track a channel selected. */
	bool Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
		   const std::vector<obs_encoder_t *> &audioEncodersByTrack,
		   const MultiStreamReconnectSettings &reconnect, std::string &error);
	bool Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
		   const MultiStreamReconnectSettings &reconnect, std::string &error);
	void Stop(bool force = false);
	bool SetChannelEnabled(const std::string &channelId, bool enabled, std::string &error);

	bool IsActive() const;
	bool HasEnabledChannels() const;
	std::vector<MultiStreamChannel> ConfiguredChannels() const;
	std::vector<MultiStreamChannelSnapshot> Snapshot() const;
	void SetStateCallback(StateCallback callback);

private:
	struct Destination {
		MultiStreamManager *owner = nullptr;
		MultiStreamChannel channel;
		OBSServiceAutoRelease service;
		OBSOutputAutoRelease output;
		MultiStreamChannelState state = MultiStreamChannelState::Idle;
		std::string lastError;
		int64_t liveSinceUnixTime = 0;
		int reconnects = 0;
		bool signalsConnected = false;
	};

	static MultiStreamChannelHealth ReadHealth(const Destination &destination);

	static void OnOutputStart(void *data, calldata_t *params);
	static void OnOutputStopping(void *data, calldata_t *params);
	static void OnOutputStop(void *data, calldata_t *params);
	static void OnOutputReconnect(void *data, calldata_t *params);

	static void ConnectSignals(Destination &destination);
	static void DisconnectSignals(Destination &destination);
	void UpdateState(Destination &destination, MultiStreamChannelState state, const char *lastError = nullptr);
	void ReportFailure(const std::vector<MultiStreamChannel> &failedChannels, const std::string &error);
	static void PersistEnabled(const std::string &channelId, bool enabled);

	/* Retires the current destinations instead of freeing them immediately:
	 * a libobs signal callback may still be running on an output thread. */
	void RetireDestinations();
	void CollectRetiredDestinations();
	bool RecomputeActiveLocked();

	mutable std::mutex mutex;
	std::vector<MultiStreamChannel> channels;
	std::string primaryChannelId;
	std::vector<std::shared_ptr<Destination>> destinations;
	std::vector<std::shared_ptr<Destination>> retiredDestinations;
	StateCallback stateCallback;
	bool active = false;
};
