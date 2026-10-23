/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <docks/OutputsDock.hpp>

#include <dialogs/MultistreamAccountsDialog.hpp>
#include <docks/UnifiedChatDock.hpp>
#include <oauth/ConnectedAccountManager.hpp>
#include <qt-wrappers.hpp>

#include <utility/MultistreamChannelStore.hpp>
#include <utility/MultistreamTaskPool.hpp>
#include <widgets/MultistreamChannelBar.hpp>

#include <QDir>
#include <QPointer>
#include <map>

#include <sstream>

void OBSBasic::ResetOutputs()
{
	ProfileScope("OBSBasic::ResetOutputs");
	std::vector<MultiStreamChannel> multistreamChannels;
	if (outputHandler && outputHandler->multiStreamManager)
		multistreamChannels = outputHandler->multiStreamManager->ConfiguredChannels();

	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool advOut = astrcmpi(mode, "Advanced") == 0;

	if ((!outputHandler || !outputHandler->Active()) &&
	    (!setupStreamingGuard.valid() ||
	     setupStreamingGuard.wait_for(std::chrono::seconds{0}) == std::future_status::ready)) {
		outputHandler.reset();
		outputHandler.reset(advOut ? CreateAdvancedOutputHandler(this) : CreateSimpleOutputHandler(this));
		if (!multistreamChannels.empty()) {
			std::string error;
			if (!outputHandler->multiStreamManager->Configure(std::move(multistreamChannels), error))
				blog(LOG_WARNING, "Could not restore multistream channels: %s", error.c_str());
		}
		BindMultistreamManager();

		emit ReplayBufEnabled(outputHandler->replayBuffer);

		if (sysTrayReplayBuffer) {
			sysTrayReplayBuffer->setEnabled(!!outputHandler->replayBuffer);
		}

		UpdateIsRecordingPausable();
	} else {
		outputHandler->Update();
	}
}

std::vector<MultiStreamManager::ChannelOutput> OBSBasic::MultistreamChannelOutputs() const
{
	if (!outputHandler || !outputHandler->multiStreamManager)
		return {};
	return outputHandler->multiStreamManager->ChannelOutputs();
}

QString OBSBasic::MultistreamPrimaryChannelName() const
{
	if (!outputHandler || !outputHandler->multiStreamManager)
		return {};
	const std::string primaryId = outputHandler->multiStreamManager->PrimaryChannelId();
	if (primaryId.empty())
		return {};
	for (const auto &channel : outputHandler->multiStreamManager->ConfiguredChannels()) {
		if (channel.id == primaryId)
			return QString::fromStdString(channel.displayName);
	}
	return {};
}

void OBSBasic::RefreshMultistreamPrimaryState()
{
	if (!outputHandler || !multistreamChannelBar)
		return;

	/* The promoted channel is carried by the application's main output, not by
	 * the manager, so no destination callback ever reports on it. Without this
	 * its card would sit on "offline" through an entire broadcast. */
	const std::string primaryId = outputHandler->multiStreamManager->PrimaryChannelId();
	if (primaryId.empty()) {
		multistreamChannelBar->SetPrimaryChannel({}, false);
		return;
	}

	MultiStreamChannelSnapshot snapshot;
	snapshot.id = primaryId;
	for (const auto &channel : outputHandler->multiStreamManager->ConfiguredChannels()) {
		if (channel.id == primaryId) {
			snapshot.displayName = channel.displayName;
			break;
		}
	}

	OBSOutputAutoRelease activeOutput = outputHandler->StreamingOutput();
	obs_output_t *output = activeOutput;
	const bool live = output && obs_output_active(output);
	multistreamChannelBar->SetPrimaryChannel(QString::fromStdString(primaryId), live);
	if (!live) {
		snapshot.state = streamingStopping ? MultiStreamChannelState::Stopping : MultiStreamChannelState::Idle;
		multistreamChannelBar->UpdateState(snapshot);
		if (outputsDock) {
			outputsDock->UpdateState(snapshot);
		}
		return;
	}

	snapshot.state = streamingStopping ? MultiStreamChannelState::Stopping : MultiStreamChannelState::Live;
	snapshot.health.congestion = obs_output_get_congestion(output);
	snapshot.health.droppedFrames = obs_output_get_frames_dropped(output);
	snapshot.health.totalFrames = obs_output_get_total_frames(output);
	snapshot.health.totalBytes = static_cast<uint64_t>(obs_output_get_total_bytes(output));
	if (multistreamPrimaryLiveSince > 0) {
		const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
					    std::chrono::system_clock::now().time_since_epoch())
					    .count();
		snapshot.health.liveSeconds = std::max<int64_t>(0, now - multistreamPrimaryLiveSince);
	}
	multistreamChannelBar->UpdateState(snapshot);
	if (outputsDock) {
		outputsDock->UpdateState(snapshot);
	}
}

static std::vector<MultiStreamChannel> PresentationChannels(std::vector<MultiStreamChannel> runtime)
{
	// Read only scoped metadata/cache on GUI. Runtime availability never replaces
	// the user's persisted enabled choice in the destination controls.
	const auto stored = MultistreamChannelStore::Load();
	for (auto &channel : runtime) {
		auto it = std::find_if(stored.begin(), stored.end(), [&](const auto &item) {
			return item.id == channel.id && item.platform == channel.platform &&
			       item.accountId == channel.accountId;
		});
		if (it != stored.end()) {
			channel.enabled = it->enabled;
		}
	}
	return runtime;
}

void OBSBasic::BindMultistreamManager()
{
	if (!outputHandler || !multistreamChannelBar)
		return;
	multistreamChannelBar->SetChannels(
		PresentationChannels(outputHandler->multiStreamManager->ConfiguredChannels()));
	/* Rebuilding the cards resets them to offline, so repaint the live state
	 * right away: this also runs while a stream is already running. */
	multistreamChannelBar->ApplySnapshots(outputHandler->multiStreamManager->Snapshot());
	RefreshMultistreamPrimaryState();
	RefreshMultistreamPreflight();
	RefreshOutputsDock();
	QPointer<MultistreamChannelBar> bar(multistreamChannelBar);
	QPointer<OutputsDock> outputs(outputsDock);
	outputHandler->multiStreamManager->SetStateCallback(
		[dispatcher = MultistreamGuiDispatcher(), bar, outputs](const MultiStreamChannelSnapshot &snapshot) {
			QMetaObject::invokeMethod(
				dispatcher,
				[bar, outputs, snapshot]() {
					if (bar) {
						bar->UpdateState(snapshot);
					}
					if (outputs) {
						outputs->UpdateState(snapshot);
					}
				},
				Qt::QueuedConnection);
		});
}

void OBSBasic::RefreshOutputsDock()
{
	if (!outputsDock || !outputHandler) {
		return;
	}
	auto &manager = outputHandler->multiStreamManager;
	outputsDock->SetChannels(PresentationChannels(manager->ConfiguredChannels()), manager->PrimaryChannelId());
	outputsDock->ApplySnapshots(manager->Snapshot());
	outputsDock->SetEncoderGroups(manager->EncoderGroups());
	outputsDock->SetLocked(StreamingActive() || manager->IsActive());
	outputsDock->SetCredentialStatus(multistreamChannelBar->CredentialStatusText());
}

void OBSBasic::ChangeMultistreamCanvas(const QString &channelId, MultiStreamVideoLayout layout)
{
	if (!outputHandler) {
		return;
	}
	if (StreamingActive() || outputHandler->multiStreamManager->IsActive()) {
		QMessageBox::information(this, QTStr("Outputs.Title"), QTStr("Multistream.ChannelBar.BusyWhileLive"));
		RefreshOutputsDock();
		return;
	}
	auto channels = MultistreamChannelStore::Load();
	auto channel = std::find_if(channels.begin(), channels.end(),
				    [&](const MultiStreamChannel &item) { return item.id == channelId.toStdString(); });
	if (channel == channels.end() || channel->videoLayout == layout) {
		RefreshOutputsDock();
		return;
	}
	channel->videoLayout = layout;
	std::string error;
	if (!MultistreamChannelStore::Save(channels, error)) {
		QMessageBox::warning(this, QTStr("Multistream.ChannelBar.UpdateFailed"), QString::fromStdString(error));
		RefreshOutputsDock();
		return;
	}
	ApplyMultistreamChannels(std::move(channels));
}

void OBSBasic::PrepareMultistreamPrimaryService()
{
	if (!outputHandler)
		return;

	auto &manager = outputHandler->multiStreamManager;
	/* A service the user configured wins: the channel bar adds destinations,
	 * it does not take over an existing setup. A service this code promoted
	 * earlier is not that, and must be recomputed instead — otherwise the
	 * channel behind it would also be fanned out and sent twice. */
	if (obs_service_can_try_to_connect(service) && !multistreamPromotedService) {
		manager->SetPrimaryChannelId({});
		return;
	}

	const MultiStreamChannel primary = manager->FirstReadyChannel();
	if (primary.id.empty()) {
		manager->SetPrimaryChannelId({});
		/* A service promoted for an earlier stream still owns that channel's
		 * server and stream key. If every channel was disabled or removed, it
		 * must be released before validation/start or OBS would silently stream
		 * to the obsolete destination. Restore the profile service (usually the
		 * empty default service) for the next attempt. */
		if (multistreamPromotedService) {
			service = nullptr;
			multistreamPromotedService = false;
			if (!LoadService()) {
				service = obs_service_create("rtmp_common", "default_service", nullptr, nullptr);
				if (service) {
					obs_service_release(service);
				}
			}
		}
		return;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "server", primary.server.c_str());
	obs_data_set_string(settings, "key", primary.streamKey.c_str());

	OBSServiceAutoRelease promoted =
		obs_service_create("rtmp_custom", "multistream_primary_service", settings, nullptr);
	if (!promoted) {
		blog(LOG_WARNING, "Could not build the main service from the multistream channel");
		return;
	}

	/* Only the in-memory service changes; SaveService() is never called here,
	 * so the Stream settings page the user sees stays untouched. */
	service = std::move(promoted);
	multistreamPromotedService = true;
	manager->SetPrimaryChannelId(primary.id);
	blog(LOG_INFO, "Multistream: using %s as the main output", primary.displayName.c_str());
}

void OBSBasic::RestoreMultistreamAccounts()
{
	if (!outputHandler || !multistreamChannelBar || MultistreamChannelStore::IsBusy()) {
		return;
	}
	const uint64_t generation = ++multistreamRestoreGeneration;

	/* Ingest keys and access tokens go stale during a long session, and the
	 * boot resolve may have failed with no network. Refreshing on a timer
	 * keeps the destinations ready without delaying the go-live click. */
	if (!multistreamCredentialTimer.isActive()) {
		multistreamCredentialTimer.setInterval(20 * 60 * 1000);
		connect(&multistreamCredentialTimer, &QTimer::timeout, this, [this]() {
			if (!outputHandler || StreamingActive() || outputHandler->multiStreamManager->IsActive())
				return;
			RestoreMultistreamAccounts();
		});
		multistreamCredentialTimer.start();
	}

	/* Preserve metadata and enabled intent while credential reads are pending.
     * Only the runtime copies without keys are temporarily unavailable. */
	if (outputHandler->multiStreamManager->ConfiguredChannels().empty()) {
		auto pending = MultistreamChannelStore::Load();
		for (auto &channel : pending) {
			if (channel.server.empty() || channel.streamKey.empty()) {
				channel.enabled = false;
			}
		}
		std::string error;
		if (!outputHandler->multiStreamManager->Configure(std::move(pending), error)) {
			multistreamChannelBar->ShowCredentialFailure(QString::fromStdString(error));
			return;
		}
		BindMultistreamManager();
	}
	multistreamChannelBar->ShowCredentialPending();
	QPointer<OBSBasic> guard(this);
	MultistreamChannelStore::LoadAsync(
		this,
		[guard, generation](MultistreamChannelStore::Result result) mutable {
			if (!guard || !guard->outputHandler || generation != guard->multistreamRestoreGeneration) {
				return;
			}
			if (!result.success) {
				guard->multistreamChannelBar->ShowCredentialFailure(
					QString::fromStdString(result.error));
				return;
			}
			if (!result.deliveryCurrent) {
				guard->multistreamChannelBar->ShowCredentialWarning(
					QString::fromStdString(result.warning));
				return;
			}
			guard->RestoreMultistreamAccountsLoaded(std::move(result.channels), generation,
								QString::fromStdString(result.warning));
		},
		[guard, generation] {
			return guard && guard->outputHandler && !guard->isClosing() &&
			       generation == guard->multistreamRestoreGeneration && !guard->StreamingActive() &&
			       !guard->outputHandler->multiStreamManager->IsActive();
		});
}

void OBSBasic::RestoreMultistreamAccountsLoaded(std::vector<MultiStreamChannel> storedChannels, uint64_t generation,
						QString credentialWarning)
{
	if (!outputHandler || generation != multistreamRestoreGeneration || StreamingActive() ||
	    outputHandler->multiStreamManager->IsActive()) {
		return;
	}
	/* Stored enabled preference is untouched when a key is missing. */
	for (auto &channel : storedChannels) {
		if (GetStreamPlatformInfo(channel.platform).ingestMode == StreamIngestMode::ManualStreamKey &&
		    channel.streamKey.empty()) {
			channel.enabled = false;
		}
	}
	std::vector<MultiStreamChannel> readyChannels;
	/* The stored channel travels with the account: resolving credentials must
	 * not reset the track selection or the broadcast metadata the user set. */
	std::vector<MultiStreamChannel> oauthChannels;
	for (auto &channel : storedChannels) {
		if (GetStreamPlatformInfo(channel.platform).ingestMode == StreamIngestMode::ManualStreamKey)
			readyChannels.emplace_back(std::move(channel));
		else
			oauthChannels.emplace_back(std::move(channel));
	}

	if (!readyChannels.empty()) {
		auto runtimePending = readyChannels;
		for (auto channel : oauthChannels) {
			channel.enabled = false;
			channel.server.clear();
			channel.streamKey.clear();
			runtimePending.push_back(std::move(channel));
		}
		std::string configureError;
		if (!outputHandler->multiStreamManager->Configure(std::move(runtimePending), configureError)) {
			blog(LOG_WARNING, "Could not restore manual multistream channels: %s", configureError.c_str());
		}
		BindMultistreamManager();
	}

	if (oauthChannels.empty()) {
		if (!credentialWarning.isEmpty()) {
			multistreamChannelBar->ShowCredentialWarning(credentialWarning);
		} else {
			multistreamChannelBar->ClearCredentialStatus();
		}
		return;
	}

	if (readyChannels.empty())
		multistreamChannelBar->ShowCredentialPending();
	QPointer<OBSBasic> guard(this);
	const QString incompleteRegistration = QTStr("Multistream.Accounts.IntegrationPending");
	auto metadataCurrent = MultistreamChannelStore::CaptureMetadataGuard();
	QObject *dispatcher = MultistreamGuiDispatcher();
	std::map<StreamPlatform, OAuthClientRegistration> registrations;
	for (const auto &channel : oauthChannels) {
		registrations[channel.platform] = MultistreamAccountsDialog::RegistrationForPlatform(channel.platform);
	}
	const quint16 kickPort = MultistreamAccountsDialog::KickCallbackPortForPlatform();
	MultistreamTaskPool().start([dispatcher, guard, metadataCurrent, registrations = std::move(registrations),
				     kickPort, generation, incompleteRegistration,
				     readyChannels = std::move(readyChannels), oauthChannels = std::move(oauthChannels),
				     credentialWarning]() mutable {
		std::vector<MultiStreamChannel> channels = std::move(readyChannels);
		std::ostringstream failures;
		if (!credentialWarning.isEmpty()) {
			failures << credentialWarning.toStdString() << '\n';
		}
		for (const auto &stored : oauthChannels) {
			auto registration = registrations.at(stored.platform);
			if (registration.clientId.empty()) {
				failures << GetStreamPlatformInfo(stored.platform).displayName << ": "
					 << QT_TO_UTF8(incompleteRegistration) << '\n';
				auto unavailable = stored;
				unavailable.enabled = false;
				unavailable.server.clear();
				unavailable.streamKey.clear();
				channels.push_back(std::move(unavailable));
				continue;
			}

			const std::string redirectUri =
				stored.platform == StreamPlatform::Kick
					? QStringLiteral("http://127.0.0.1:%1/").arg(kickPort).toStdString()
					: std::string{};
			const ConnectedStreamAccount account{stored.platform, stored.accountId, stored.displayName,
							     stored.enabled};
			MultiStreamChannel channel;
			std::string error;
			if (MultistreamAccountsDialog::LoadRegistrationCredential(stored.platform, registration,
										  error) &&
			    ConnectedAccountManager::ResolveChannel(account, registration, redirectUri, channel,
								    error)) {
				ConnectedAccountManager::PreserveUserSettings(stored, channel);
				channels.emplace_back(std::move(channel));
			} else {
				failures << GetStreamPlatformInfo(stored.platform).displayName << ": " << error << '\n';
				auto unavailable = stored;
				unavailable.enabled = false;
				unavailable.server.clear();
				unavailable.streamKey.clear();
				channels.push_back(std::move(unavailable));
			}
		}

		const QString failureText = QString::fromStdString(failures.str()).trimmed();
		QMetaObject::invokeMethod(
			dispatcher,
			[guard, generation, metadataCurrent, channels = std::move(channels), failureText]() mutable {
				if (!guard || !guard->outputHandler ||
				    generation != guard->multistreamRestoreGeneration || guard->StreamingActive() ||
				    guard->outputHandler->multiStreamManager->IsActive())
					return;
				if (!metadataCurrent()) {
					guard->multistreamChannelBar->ShowCredentialWarning(
						QTStr("Multistream.Credentials.StateChanged"));
					return;
				}
				std::string error;
				if (!guard->outputHandler->multiStreamManager->Configure(std::move(channels), error)) {
					guard->multistreamChannelBar->ShowCredentialFailure(
						QString::fromStdString(error));
					return;
				}
				guard->BindMultistreamManager();
				/* The resolve is the only place the platform handle
				 * and avatar are learned, and chat reads them back
				 * from the store, so they have to be written down. */
				QString finalWarning = failureText;
				std::string identityError;
				if (!MultistreamChannelStore::UpdateIdentity(
					    guard->outputHandler->multiStreamManager->ConfiguredChannels(),
					    identityError)) {
					finalWarning += "\n" + QString::fromStdString(identityError);
					blog(LOG_WARNING, "Could not store the resolved channel identities: %s",
					     identityError.c_str());
				}
				/* Chat may have already connected using whatever the
				 * store held before the resolve; point it at the
				 * handle the platform just confirmed. */
				if (guard->unifiedChatDock && guard->unifiedChatDock->isVisible())
					guard->unifiedChatDock->AutoConnectAccounts();
				if (guard->outputHandler->multiStreamManager->ConfiguredChannels().empty() &&
				    !finalWarning.isEmpty()) {
					guard->multistreamChannelBar->ShowCredentialFailure(finalWarning);
				} else if (!finalWarning.isEmpty()) {
					guard->multistreamChannelBar->ShowCredentialWarning(finalWarning);
					blog(LOG_WARNING, "Some multistream accounts could not be restored: %s",
					     QT_TO_UTF8(failureText));
				} else {
					guard->multistreamChannelBar->ClearCredentialStatus();
				}
			},
			Qt::QueuedConnection);
	});
}

bool OBSBasic::Active() const
{
	if (!outputHandler) {
		return false;
	}
	return outputHandler->Active();
}

void OBSBasic::ResizeOutputSizeOfSource()
{
	if (obs_video_active()) {
		return;
	}

	QMessageBox resize_output(this);
	resize_output.setText(QTStr("ResizeOutputSizeOfSource.Text") + "\n\n" +
			      QTStr("ResizeOutputSizeOfSource.Continue"));
	QAbstractButton *Yes = resize_output.addButton(QTStr("Yes"), QMessageBox::YesRole);
	resize_output.addButton(QTStr("No"), QMessageBox::NoRole);
	resize_output.setIcon(QMessageBox::Warning);
	resize_output.setWindowTitle(QTStr("ResizeOutputSizeOfSource"));
	resize_output.exec();

	if (resize_output.clickedButton() != Yes) {
		return;
	}

	OBSSource source = obs_sceneitem_get_source(GetCurrentSceneItem());

	int width = obs_source_get_width(source);
	int height = obs_source_get_height(source);

	width = ((width + 3) / 4) * 4;   // Round width up to the nearest multiple of 4
	height = ((height + 1) / 2) * 2; // Round height up to the nearest multiple of 2

	config_set_uint(activeConfiguration, "Video", "BaseCX", width);
	config_set_uint(activeConfiguration, "Video", "BaseCY", height);
	config_set_uint(activeConfiguration, "Video", "OutputCX", width);
	config_set_uint(activeConfiguration, "Video", "OutputCY", height);

	ResetVideo();
	ResetOutputs();
	activeConfiguration.SaveSafe("tmp");
	on_actionFitToScreen_triggered();
}

const char *OBSBasic::GetCurrentOutputPath()
{
	const char *path = nullptr;
	const char *mode = config_get_string(Config(), "Output", "Mode");

	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");

		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			path = config_get_string(Config(), "AdvOut", "FFFilePath");
		} else {
			path = config_get_string(Config(), "AdvOut", "RecFilePath");
		}
	} else {
		path = config_get_string(Config(), "SimpleOutput", "FilePath");
	}

	return path;
}

void OBSBasic::OutputPathInvalidMessage()
{
	blog(LOG_ERROR, "Recording stopped because of bad output path");

	OBSMessageBox::critical(this, QTStr("Output.BadPath.Title"), QTStr("Output.BadPath.Text"));
}

bool OBSBasic::IsFFmpegOutputToURL() const
{
	const char *mode = config_get_string(Config(), "Output", "Mode");
	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");
		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			bool is_local = config_get_bool(Config(), "AdvOut", "FFOutputToFile");
			if (!is_local) {
				return true;
			}
		}
	}

	return false;
}

bool OBSBasic::OutputPathValid()
{
	if (IsFFmpegOutputToURL()) {
		return true;
	}

	const char *path = GetCurrentOutputPath();
	return path && *path && QDir(path).exists();
}
