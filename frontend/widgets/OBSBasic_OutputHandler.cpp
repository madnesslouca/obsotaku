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

#include <dialogs/MultistreamAccountsDialog.hpp>
#include <docks/UnifiedChatDock.hpp>
#include <oauth/ConnectedAccountManager.hpp>
#include <qt-wrappers.hpp>

#include <utility/MultistreamChannelStore.hpp>
#include <utility/MultistreamTaskPool.hpp>
#include <widgets/MultistreamChannelBar.hpp>

#include <QDir>
#include <QPointer>

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

void OBSBasic::BindMultistreamManager()
{
	if (!outputHandler || !multistreamChannelBar)
		return;
	multistreamChannelBar->SetChannels(outputHandler->multiStreamManager->ConfiguredChannels());
	/* Rebuilding the cards resets them to offline, so repaint the live state
	 * right away: this also runs while a stream is already running. */
	multistreamChannelBar->ApplySnapshots(outputHandler->multiStreamManager->Snapshot());
	RefreshMultistreamPreflight();
	QPointer<MultistreamChannelBar> bar(multistreamChannelBar);
	outputHandler->multiStreamManager->SetStateCallback([bar](const MultiStreamChannelSnapshot &snapshot) {
		if (!bar)
			return;
		QMetaObject::invokeMethod(
			bar.data(), [bar, snapshot]() {
				if (bar)
					bar->UpdateState(snapshot);
			},
			Qt::QueuedConnection);
	});
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
	if (!outputHandler || !multistreamChannelBar)
		return;

	/* Ingest keys and access tokens go stale during a long session, and the
	 * boot resolve may have failed with no network. Refreshing on a timer
	 * keeps the destinations ready without delaying the go-live click. */
	if (!multistreamCredentialTimer.isActive()) {
		multistreamCredentialTimer.setInterval(20 * 60 * 1000);
		connect(&multistreamCredentialTimer, &QTimer::timeout, this, [this]() {
			if (!outputHandler || outputHandler->multiStreamManager->IsActive())
				return;
			RestoreMultistreamAccounts();
		});
		multistreamCredentialTimer.start();
	}

	/* Manual destinations already carry their stream key, so they go live
	 * immediately; only the OAuth ones need a network round trip. */
	std::vector<MultiStreamChannel> storedChannels = MultistreamChannelStore::Load();
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
		std::string configureError;
		if (!outputHandler->multiStreamManager->Configure(readyChannels, configureError))
			blog(LOG_WARNING, "Could not restore manual multistream channels: %s", configureError.c_str());
		BindMultistreamManager();
	}

	if (oauthChannels.empty())
		return;

	if (readyChannels.empty())
		multistreamChannelBar->ShowRestoringAccounts();
	QPointer<OBSBasic> guard(this);
	const QString incompleteRegistration = QTStr("Multistream.Accounts.IntegrationPending");
	MultistreamTaskPool().start([guard, incompleteRegistration, readyChannels = std::move(readyChannels),
				     oauthChannels = std::move(oauthChannels)]() mutable {
		std::vector<MultiStreamChannel> channels = std::move(readyChannels);
		std::ostringstream failures;
		for (const auto &stored : oauthChannels) {
			const auto registration = MultistreamAccountsDialog::RegistrationForPlatform(stored.platform);
			if (!MultistreamAccountsDialog::RegistrationReadyForPlatform(stored.platform)) {
				failures << GetStreamPlatformInfo(stored.platform).displayName << ": "
					 << QT_TO_UTF8(incompleteRegistration) << '\n';
				continue;
			}

			const std::string redirectUri =
				stored.platform == StreamPlatform::Kick
					? QStringLiteral("http://127.0.0.1:%1/")
						  .arg(MultistreamAccountsDialog::KickCallbackPortForPlatform())
						  .toStdString()
					: std::string{};
			const ConnectedStreamAccount account{stored.platform, stored.accountId, stored.displayName,
							     stored.enabled};
			MultiStreamChannel channel;
			std::string error;
			if (ConnectedAccountManager::ResolveChannel(account, registration, redirectUri, channel,
							    error)) {
				ConnectedAccountManager::PreserveUserSettings(stored, channel);
				channels.emplace_back(std::move(channel));
			} else {
				failures << GetStreamPlatformInfo(stored.platform).displayName << ": " << error << '\n';
			}
		}

		if (!guard)
			return;
		const QString failureText = QString::fromStdString(failures.str()).trimmed();
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, channels = std::move(channels), failureText]() mutable {
				if (!guard || !guard->outputHandler ||
				    guard->outputHandler->multiStreamManager->IsActive())
					return;
				std::string error;
				if (!guard->outputHandler->multiStreamManager->Configure(std::move(channels), error)) {
					guard->multistreamChannelBar->ShowRestoreFailure(
						QString::fromStdString(error));
					return;
				}
				guard->BindMultistreamManager();
				/* The resolve is the only place the platform handle
				 * and avatar are learned, and chat reads them back
				 * from the store, so they have to be written down. */
				std::string identityError;
				if (!MultistreamChannelStore::UpdateIdentity(
					    guard->outputHandler->multiStreamManager->ConfiguredChannels(),
					    identityError))
					blog(LOG_WARNING, "Could not store the resolved channel identities: %s",
					     identityError.c_str());
				/* Chat may have already connected using whatever the
				 * store held before the resolve; point it at the
				 * handle the platform just confirmed. */
				if (guard->unifiedChatDock && guard->unifiedChatDock->isVisible())
					guard->unifiedChatDock->AutoConnectAccounts();
				if (guard->outputHandler->multiStreamManager->ConfiguredChannels().empty() &&
				    !failureText.isEmpty())
					guard->multistreamChannelBar->ShowRestoreFailure(failureText);
				else if (!failureText.isEmpty())
					blog(LOG_WARNING, "Some multistream accounts could not be restored: %s",
					     QT_TO_UTF8(failureText));
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
