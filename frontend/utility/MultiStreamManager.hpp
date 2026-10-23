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
#include <QObject>

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

enum class MultiStreamVideoLayout {
	Main,
	Portrait,
};

enum class MultiStreamPortraitFit {
	/* Show the complete horizontal program with empty space above and below. */
	Fit,
	/* Fill the phone screen and crop the sides of the horizontal program. */
	Fill,
};

struct MultiStreamChannel {
	std::string id;
	std::string displayName;
	StreamPlatform platform = StreamPlatform::CustomRtmp;
	std::string accountId;
	/* Handle the platform's chat is addressed by: the Twitch login, the Kick
	 * slug. Kept apart from displayName because the user may rename a channel
	 * and that must not break chat. */
	std::string chatAddress;
	std::string server;
	std::string streamKey;
	/* Remote profile picture URL; the channel bar caches it on disk. */
	std::string avatarUrl;
	/* Broadcast metadata. An empty title means the channel follows the shared
	 * one from the stream info panel. */
	std::string title;
	std::string categoryId;
	std::string categoryName;
	/* Portrait destinations use their own 9:16 canvas and video encoder. */
	MultiStreamVideoLayout videoLayout = MultiStreamVideoLayout::Main;
	MultiStreamPortraitFit portraitFit = MultiStreamPortraitFit::Fill;
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

/* Additional outputs borrow a canvas encoder which may also serve the primary
 * stream, recording or replay. Only the primary output may adapt that encoder;
 * counting destinations here cannot establish exclusive ownership. */
enum class MultiStreamDynamicBitrateReason {
	NotRequested,
	SharedEncoderAuthority,
};

struct MultiStreamChannelSnapshot {
	std::string id;
	std::string displayName;
	MultiStreamChannelState state = MultiStreamChannelState::Idle;
	std::string lastError;
	MultiStreamChannelHealth health;
	bool dynamicBitrateRequested = false;
	/* This is the additional output's effective policy, not a promise that
	 * another owner (e.g. the primary stream) cannot adapt the shared encoder. */
	bool dynamicBitrateEffective = false;
	MultiStreamDynamicBitrateReason dynamicBitrateReason = MultiStreamDynamicBitrateReason::NotRequested;
};

struct MultiStreamReconnectSettings {
	int maxRetries = 20;
	int retryDelaySeconds = 2;
};

/* Network behaviour shared with OBS' primary stream output. Keeping these
 * values together prevents secondary destinations from silently ignoring the
 * selected adapter, stream delay, or dynamic-bitrate policy. */
struct MultiStreamOutputSettings {
	int portraitBitrateKbps = 6000;
	MultiStreamReconnectSettings reconnect;
	std::string bindIp;
	std::string ipFamily;
	int delaySeconds = 0;
	bool preserveDelay = false;
	/* Persisted user intent. Additional outputs report a restricted effective
	 * policy without overwriting this preference or creating new encoders. */
	bool dynamicBitrate = false;
	bool newSocketLoop = false;
	bool lowLatency = false;
};

/* Control operations run on the owning frontend thread. Output signals may
 * arrive from libobs threads; deferred restarts return to the owner's thread. */
class MultiStreamManager : public QObject {
public:
	using StateCallback = std::function<void(const MultiStreamChannelSnapshot &)>;

	MultiStreamManager() = default;
	~MultiStreamManager() override;

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

	/* Returns the encoder the primary output must use. For a portrait primary
	 * this prepares the 9:16 canvas before OBS starts its main output. */
	obs_encoder_t *PreparePrimaryVideoEncoder(obs_encoder_t *mainVideoEncoder, std::string &error, int portraitBitrateKbps = 6000);

	/* audioEncodersByTrack is indexed by OBS audio track, so entries may be
	 * null. Compacting it would silently remap the track a channel selected. */
	bool Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
		   const std::vector<obs_encoder_t *> &audioEncodersByTrack,
		   const MultiStreamOutputSettings &outputSettings, std::string &error);
	bool Start(obs_encoder_t *videoEncoder, obs_encoder_t *defaultAudioEncoder,
		   const MultiStreamOutputSettings &outputSettings, std::string &error);
	void Stop(bool force = false);
	bool SetChannelEnabled(const std::string &channelId, bool enabled, std::string &error);
	/* Restart only this destination after its asynchronous stop completes.
	 * Does not change the stored enabled flag or restart shared encoders. */
	bool RestartChannel(const std::string &channelId, std::string &error);

	bool IsActive() const;
	/* True when an additional destination can be prepared. It deliberately
	 * includes disabled channels so the user can enable one while already
	 * live without getting a silent no-op. */
	bool HasAdditionalChannels() const;
	std::vector<MultiStreamChannel> ConfiguredChannels() const;
	std::vector<MultiStreamChannelSnapshot> Snapshot() const;

	/* One entry per destination that currently owns an output, so the
	 * statistics window can read bitrate and dropped frames straight from
	 * libobs. Each output carries a reference of its own. */
	struct ChannelOutput {
		std::string id;
		std::string displayName;
		OBSOutputAutoRelease output;
	};
	std::vector<ChannelOutput> ChannelOutputs() const;
	void SetStateCallback(StateCallback callback);

	/* Dual-canvas scenes: when a provider is installed and returns a canvas,
	 * portrait destinations encode that persistent 9:16 canvas (which has its
	 * own per-scene layout) instead of a private mirror of the main program.
	 * The provider must be callable from any thread. */
	using PortraitCanvasProvider = std::function<OBSCanvas()>;
	static void SetPortraitCanvasProvider(PortraitCanvasProvider provider);

	/* Which video encoder each canvas uses, for the outputs panel. Every
	 * destination on the same canvas shares one encoder. */
	struct EncoderGroup {
		MultiStreamVideoLayout layout = MultiStreamVideoLayout::Main;
		std::string encoderId;
		std::vector<std::string> channelIds;
	};
	std::vector<EncoderGroup> EncoderGroups() const;

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
		bool restartPending = false;
		bool restartQueued = false;
		bool stopPending = false;
		bool dynamicBitrateRequested = false;
	};

	static MultiStreamChannelHealth ReadHealth(const Destination &destination);

	static void OnOutputStart(void *data, calldata_t *params);
	static void OnOutputStopping(void *data, calldata_t *params);
	static void OnOutputStop(void *data, calldata_t *params);
	static void OnOutputReconnect(void *data, calldata_t *params);

	static void ConnectSignals(Destination &destination);
	static void DisconnectSignals(Destination &destination);
	void UpdateState(Destination &destination, MultiStreamChannelState state, const char *lastError = nullptr,
			 bool countReconnect = false);
	void ReportFailure(const std::vector<MultiStreamChannel> &failedChannels, const std::string &error);
	static bool PersistEnabled(const std::string &channelId, bool enabled, std::string &error);
	static void OnMainChannelChanged(void *data, calldata_t *params);
	bool PreparePortraitPipeline(obs_encoder_t *mainVideoEncoder, MultiStreamPortraitFit fit, std::string &error);
	void SetPortraitSource(obs_source_t *source, MultiStreamPortraitFit fit);
	void DestroyPortraitPipeline();
	static std::mutex &PortraitProviderMutex();
	static PortraitCanvasProvider &PortraitProvider();

	/* Retires the current destinations instead of freeing them immediately:
	 * a libobs signal callback may still be running on an output thread. */
	void RetireDestinations();
	void CollectRetiredDestinations();
	void QueueRestart(Destination &destination);
	void ResumeRestart(const std::weak_ptr<Destination> &destination);
	/* State notifications are reentrant: Stop/disable/Configure may run from
	 * the Starting callback before control reaches obs_output_start(). */
	bool CanStartDestination(const std::shared_ptr<Destination> &destination, uint64_t generation) const;
	bool RecomputeActiveLocked();

	mutable std::mutex mutex;
	std::vector<MultiStreamChannel> channels;
	std::string primaryChannelId;
	std::vector<std::shared_ptr<Destination>> destinations;
	std::vector<std::shared_ptr<Destination>> retiredDestinations;
	StateCallback stateCallback;
	OBSCanvasAutoRelease portraitCanvas;
	OBSSceneAutoRelease portraitScene;
	OBSEncoderAutoRelease portraitVideoEncoder;
	OBSEncoderAutoRelease sessionMainVideoEncoder;
	obs_sceneitem_t *portraitSceneItem = nullptr;
	/* The portrait canvas belongs to the scene collection, not to us. */
	int portraitBitrateKbps = 6000;
	bool portraitCanvasShared = false;
	MultiStreamPortraitFit portraitFit = MultiStreamPortraitFit::Fill;
	bool mainChannelSignalConnected = false;
	bool active = false;
	bool sessionRunning = false;
	uint64_t sessionGeneration = 0;
};
