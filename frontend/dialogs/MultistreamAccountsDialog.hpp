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
#include <QPointer>
#include <QTimer>

#include <array>
#include <optional>
#include <vector>

class AuthListener;
class QLabel;
class QPushButton;
class QVBoxLayout;

/* Connects and manages OAuth accounts. It works on channels, not platforms, so
 * the same platform can hold several accounts: each connection produces its own
 * channel keyed by the account id the platform returned. */
class MultistreamAccountsDialog : public QDialog {
	Q_OBJECT

public:
	/* existingChannels are the OAuth channels to manage. Pass newAccountPlatform
	 * to also offer an empty card for connecting another account on that
	 * platform, which is what picking it in the add-channel grid means. */
	MultistreamAccountsDialog(QWidget *parent, std::vector<MultiStreamChannel> existingChannels,
				  std::optional<StreamPlatform> newAccountPlatform = std::nullopt);

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
	struct AccountCard {
		StreamPlatform platform = StreamPlatform::CustomRtmp;
		QLabel *status = nullptr;
		class QButtonGroup *audioTrackGroup = nullptr;
		class QCheckBox *vodTrackCheckBox = nullptr;
		class QButtonGroup *vodTrackGroup = nullptr;
		std::array<class QRadioButton *, 6> audioTrackRadios{};
		std::array<class QRadioButton *, 6> vodTrackRadios{};
		QPushButton *connectButton = nullptr;
		QPushButton *configureButton = nullptr;
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

	AccountCard *CardAt(int index);
	AccountCard *CardForPlatform(StreamPlatform platform);
	void AddAccountCard(int index, QVBoxLayout *layout);
	void UpdateCard(AccountCard &card);
	void ResolveChannelInBackground(int index);

	void ConnectAccount(int index);
	void ConfigureKickRegistration();
	void StartPkceConnection(int index, const OAuthClientRegistration &registration);
	void StartTwitchConnection(int index, const OAuthClientRegistration &registration);
	void PollTwitch();
	void RefreshConnectedAccount(int index, const OAuthClientRegistration &registration);
	void DisconnectAccount(int index);

	void SetBusy(int index, const QString &status);
	void CancelPendingConnection();
	void FinishConnection(int index, bool success, ConnectedStreamAccount account, MultiStreamChannel channel,
			      const QString &error);
	void ClearLoopback();

	std::vector<AccountCard> cards;
	QLabel *instructions = nullptr;
	QLabel *emptyHint = nullptr;
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
	/* Index of the card currently connecting, or -1. */
	int busyIndex = -1;
};
