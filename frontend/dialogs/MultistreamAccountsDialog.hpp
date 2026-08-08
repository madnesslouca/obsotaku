/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <oauth/ConnectedAccountManager.hpp>
#include <oauth/PlatformOAuthClient.hpp>
#include <utility/StreamPlatform.hpp>

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class AuthListener;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

/* Connects and manages OAuth accounts. Accounts are grouped under their
 * platform, one compact row each, so a platform can hold several without the
 * dialog turning into a stack of identical cards. */
class MultistreamAccountsDialog : public QDialog {
	Q_OBJECT

public:
	/* existingChannels are the OAuth channels to manage. Pass focusedPlatform to
	 * show that platform alone — what both picking it in the add-channel grid
	 * and managing one channel's account mean. Left empty, every platform gets
	 * a group so a first account can be connected from here. */
	MultistreamAccountsDialog(QWidget *parent, std::vector<MultiStreamChannel> existingChannels,
				  std::optional<StreamPlatform> focusedPlatform = std::nullopt);

	/* Channels this dialog produced, including edits. */
	std::vector<MultiStreamChannel> Channels() const;

	/* Ids the dialog was responsible for. The caller replaces exactly these in
	 * the store, so a focused dialog never drops the other accounts. */
	std::vector<std::string> ManagedChannelIds() const;

	static OAuthClientRegistration RegistrationForPlatform(StreamPlatform platform);
	static bool RegistrationReadyForPlatform(StreamPlatform platform);
	static quint16 KickCallbackPortForPlatform();

protected:
	void reject() override;

private:
	/* Held by pointer so rows added at runtime never invalidate the others. */
	struct AccountRow {
		StreamPlatform platform = StreamPlatform::CustomRtmp;
		QWidget *widget = nullptr;
		QLabel *nameLabel = nullptr;
		QLabel *statusLabel = nullptr;
		QComboBox *audioTrackCombo = nullptr;
		QCheckBox *vodTrackCheckBox = nullptr;
		QComboBox *vodTrackCombo = nullptr;
		QPushButton *connectButton = nullptr;
		QPushButton *disconnectButton = nullptr;
		ConnectedStreamAccount account;
		MultiStreamChannel channel;
		bool connected = false;
		/* False until the platform returned a server and stream key. An
		 * unresolved channel must never be handed to the output layer. */
		bool credentialsResolved = false;
		/* Set when the user disconnects: the caller drops it from the store. */
		bool removed = false;
	};

	AccountRow *RowAt(int index);

	void BuildPlatformGroup(StreamPlatform platform, const std::vector<int> &rowIndexes, QVBoxLayout *layout);
	QWidget *BuildAccountRow(int index);
	int AddPendingAccount(StreamPlatform platform);
	void UpdateRow(AccountRow &row);
	void UpdateGroupHint(StreamPlatform platform);
	void ResolveChannelInBackground(int index);

	void ConnectAccount(int index);
	void ConfigureKickRegistration();
	void StartPkceConnection(int index, const OAuthClientRegistration &registration);
	void StartTwitchConnection(int index, const OAuthClientRegistration &registration);
	void PollTwitch();
	void RefreshConnectedAccount(int index, const OAuthClientRegistration &registration);
	void DisconnectAccount(int index);

	void SetBusy(int index, const QString &status);
	void SetControlsEnabled(bool enabled);
	void CancelPendingConnection();
	void FinishConnection(int index, bool success, ConnectedStreamAccount account, MultiStreamChannel channel,
			      const QString &error);
	void ClearLoopback();

	std::vector<std::unique_ptr<AccountRow>> rows;
	/* Where new rows are inserted, and the label shown when a group is empty. */
	QHash<int, QVBoxLayout *> groupLayouts;
	QHash<int, QLabel *> groupHints;
	QLabel *instructions = nullptr;
	QPushButton *closeButton = nullptr;
	QPushButton *cancelButton = nullptr;
	QPointer<AuthListener> loopback;
	QTimer twitchPollTimer;
	QTimer connectionTimeout;
	QElapsedTimer twitchElapsed;
	OAuthDeviceAuthorization twitchAuthorization;
	OAuthClientRegistration twitchRegistration;
	int twitchPollIntervalSeconds = 5;
	bool busy = false;
	/* Index of the row currently connecting, or -1. */
	int busyIndex = -1;
};
