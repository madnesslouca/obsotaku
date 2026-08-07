/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamAccountsDialog.hpp"
#include "ui-config.h"

#include <oauth/AuthListener.hpp>
#include <utility/MultistreamTaskPool.hpp>
#include <utility/PlatformIconProvider.hpp>
#include <utility/SecureTokenStore.hpp>
#include <utility/StreamPlatformDisplay.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <util/config-file.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

#include "moc_MultistreamAccountsDialog.cpp"

using namespace std;

namespace {
constexpr const char *KICK_CLIENT_CREDENTIAL_PLATFORM = "kick-client";
constexpr int AUDIO_TRACK_COUNT = 6;
/* Long enough for a slow login with two-factor, short enough that a forgotten
 * browser tab does not leave the dialog unusable forever. */
constexpr int CONNECTION_TIMEOUT_MS = 5 * 60 * 1000;

struct ConnectionResult {
	bool success = false;
	ConnectedStreamAccount account;
	MultiStreamChannel channel;
	QString error;
};

struct DeviceStartResult {
	bool success = false;
	OAuthDeviceAuthorization authorization;
	QString error;
};

struct DevicePollResult {
	OAuthDevicePollStatus status = OAuthDevicePollStatus::Error;
	ConnectionResult connection;
	QString error;
};

const char *ClientIdEnvironmentName(StreamPlatform platform)
{
	switch (platform) {
	case StreamPlatform::YouTube:
		return "OBS_MULTISTREAM_YOUTUBE_CLIENT_ID";
	case StreamPlatform::Twitch:
		return "OBS_MULTISTREAM_TWITCH_CLIENT_ID";
	case StreamPlatform::Kick:
		return "OBS_MULTISTREAM_KICK_CLIENT_ID";
	default:
		break;
	}
	return "";
}

OAuthClientRegistration RegistrationFor(StreamPlatform platform)
{
	OAuthClientRegistration registration;
	registration.clientId = qEnvironmentVariable(ClientIdEnvironmentName(platform)).toStdString();
	if (registration.clientId.empty()) {
		config_t *config = App()->GetUserConfig();
		const string section = StreamPlatformConfigSection(platform);
		const char *cfgClientId = config_get_string(config, section.c_str(), "ClientId");
		if (cfgClientId && *cfgClientId)
			registration.clientId = cfgClientId;
	}
	if (registration.clientId.empty()) {
		switch (platform) {
		case StreamPlatform::YouTube:
			registration.clientId = PRODUCT_YOUTUBE_CLIENT_ID;
			break;
		case StreamPlatform::Twitch:
			registration.clientId = PRODUCT_TWITCH_CLIENT_ID;
			break;
		case StreamPlatform::Kick:
			registration.clientId = PRODUCT_KICK_CLIENT_ID;
			break;
		default:
			break;
		}
	}
	if (platform != StreamPlatform::Kick)
		return registration;

	registration.tokenExchangeEndpoint = qEnvironmentVariable("OBS_MULTISTREAM_KICK_TOKEN_PROXY").toStdString();
	if (registration.tokenExchangeEndpoint.empty())
		registration.tokenExchangeEndpoint = PRODUCT_KICK_TOKEN_PROXY;
	if (registration.tokenExchangeEndpoint.empty() && !registration.clientId.empty()) {
		string credentialError;
		auto clientSecret =
			SecureTokenStore::Load(KICK_CLIENT_CREDENTIAL_PLATFORM, registration.clientId, credentialError);
		if (clientSecret)
			registration.clientSecret = std::move(*clientSecret);
		else if (!credentialError.empty())
			blog(LOG_WARNING, "Could not load the local Kick client credential: %s",
			     credentialError.c_str());
	}
	return registration;
}

bool RegistrationReady(StreamPlatform platform)
{
	const auto registration = RegistrationFor(platform);
	return !registration.clientId.empty() &&
	       (platform != StreamPlatform::Kick || !registration.tokenExchangeEndpoint.empty() ||
		!registration.clientSecret.empty());
}

QString RegistrationStatusText(StreamPlatform platform)
{
	const auto registration = RegistrationFor(platform);
	if (registration.clientId.empty())
		return QTStr("Multistream.Accounts.IntegrationPending");
	if (platform == StreamPlatform::Kick && registration.tokenExchangeEndpoint.empty() &&
	    registration.clientSecret.empty())
		return QTStr("Multistream.Accounts.KickProxyPending");
	return QTStr("Multistream.Accounts.NotConnected");
}

QString RegistrationToolTip(StreamPlatform platform)
{
	const auto registration = RegistrationFor(platform);
	if (registration.clientId.empty())
		return QTStr("Multistream.Accounts.RegistrationHelp");
	if (platform == StreamPlatform::Kick && registration.tokenExchangeEndpoint.empty() &&
	    registration.clientSecret.empty())
		return QTStr("Multistream.Accounts.MissingKickProxy");
	return {};
}

quint16 KickCallbackPort()
{
	bool valid = false;
	const uint value = qEnvironmentVariableIntValue("OBS_MULTISTREAM_KICK_CALLBACK_PORT", &valid);
	return valid && value > 0 && value <= 65535 ? static_cast<quint16>(value) : 49327;
}

string KickRedirectUri()
{
	return QStringLiteral("http://127.0.0.1:%1/").arg(KickCallbackPort()).toStdString();
}

string RedirectUriFor(StreamPlatform platform)
{
	/* Only Kick pins a fixed loopback port, because its developer portal
	 * requires the redirect URI to be registered up front. */
	return platform == StreamPlatform::Kick ? KickRedirectUri() : string{};
}

QString FromStdString(const string &value)
{
	return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace

OAuthClientRegistration MultistreamAccountsDialog::RegistrationForPlatform(StreamPlatform platform)
{
	return RegistrationFor(platform);
}

bool MultistreamAccountsDialog::RegistrationReadyForPlatform(StreamPlatform platform)
{
	return RegistrationReady(platform);
}

quint16 MultistreamAccountsDialog::KickCallbackPortForPlatform()
{
	return KickCallbackPort();
}

MultistreamAccountsDialog::MultistreamAccountsDialog(QWidget *parent, vector<MultiStreamChannel> existingChannels,
						     optional<StreamPlatform> newAccountPlatform)
	: QDialog(parent)
{
	setWindowTitle(newAccountPlatform
			       ? QTStr("Multistream.Accounts.SingleTitle").arg(
					 StreamPlatformDisplayName(*newAccountPlatform))
			       : QTStr("Multistream.Accounts.Title"));
	setMinimumWidth(640);
	setModal(true);
	/* Styling lives in the theme files (see Yami.obt). A widget stylesheet
	 * here would take precedence over the active theme and break light mode. */
	setObjectName(QStringLiteral("multistreamAccountsDialog"));

	auto *layout = new QVBoxLayout(this);
	/* The device-code instructions appear mid-flow; without this the dialog
	 * keeps its original height and the rows overlap. */
	layout->setSizeConstraint(QLayout::SetMinimumSize);

	auto *subtitle = new QLabel(QTStr("Multistream.Accounts.Subtitle"), this);
	subtitle->setObjectName(QStringLiteral("infoBanner"));
	subtitle->setWordWrap(true);
	layout->addWidget(subtitle);

	/* One card per connected account, so a platform can hold several. */
	for (auto &channel : existingChannels) {
		if (GetStreamPlatformInfo(channel.platform).ingestMode != StreamIngestMode::ResolvedByApi)
			continue;
		if (newAccountPlatform && channel.platform != *newAccountPlatform)
			continue;

		AccountCard card;
		card.platform = channel.platform;
		card.account = {channel.platform, channel.accountId, channel.displayName, channel.enabled};
		card.connected = !channel.accountId.empty();
		card.credentialsResolved = !channel.server.empty() && !channel.streamKey.empty();
		card.channel = std::move(channel);
		cards.push_back(std::move(card));
	}

	/* An empty card for connecting another account on this platform. */
	if (newAccountPlatform) {
		AccountCard card;
		card.platform = *newAccountPlatform;
		card.channel.platform = *newAccountPlatform;
		cards.push_back(std::move(card));
	}

	for (int index = 0; index < static_cast<int>(cards.size()); ++index)
		AddAccountCard(index, layout);

	emptyHint = new QLabel(QTStr("Multistream.Accounts.NoAccounts"), this);
	emptyHint->setObjectName(QStringLiteral("infoBanner"));
	emptyHint->setWordWrap(true);
	emptyHint->setVisible(cards.empty());
	layout->addWidget(emptyHint);

	instructions = new QLabel(this);
	instructions->setWordWrap(true);
	instructions->setTextInteractionFlags(Qt::TextSelectableByMouse);
	instructions->setVisible(false);
	layout->addWidget(instructions);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	closeButton = buttons->button(QDialogButtonBox::Close);
	cancelButton = buttons->addButton(QTStr("Multistream.Accounts.Cancel"), QDialogButtonBox::ResetRole);
	cancelButton->setVisible(false);
	connect(cancelButton, &QPushButton::clicked, this, &MultistreamAccountsDialog::CancelPendingConnection);
	connect(buttons, &QDialogButtonBox::rejected, this, &MultistreamAccountsDialog::reject);
	layout->addWidget(buttons);

	twitchPollTimer.setSingleShot(true);
	connect(&twitchPollTimer, &QTimer::timeout, this, &MultistreamAccountsDialog::PollTwitch);

	connectionTimeout.setSingleShot(true);
	connect(&connectionTimeout, &QTimer::timeout, this, [this]() {
		if (!busy || busyIndex < 0)
			return;
		ClearLoopback();
		twitchPollTimer.stop();
		FinishConnection(busyIndex, false, {}, {}, QTStr("Multistream.Accounts.TimedOut"));
	});

	for (int index = 0; index < static_cast<int>(cards.size()); ++index) {
		if (cards[index].connected && !cards[index].credentialsResolved)
			ResolveChannelInBackground(index);
	}
}

MultistreamAccountsDialog::AccountCard *MultistreamAccountsDialog::CardAt(int index)
{
	if (index < 0 || index >= static_cast<int>(cards.size()))
		return nullptr;
	return &cards[static_cast<size_t>(index)];
}

MultistreamAccountsDialog::AccountCard *MultistreamAccountsDialog::CardForPlatform(StreamPlatform platform)
{
	auto card = find_if(cards.begin(), cards.end(),
			    [platform](const AccountCard &item) { return item.platform == platform; });
	return card != cards.end() ? &*card : nullptr;
}

vector<MultiStreamChannel> MultistreamAccountsDialog::Channels() const
{
	vector<MultiStreamChannel> channels;
	for (const auto &card : cards) {
		if (card.removed || !card.connected || card.account.accountId.empty())
			continue;

		MultiStreamChannel channel = card.channel;
		if (channel.id.empty()) {
			const auto &info = GetStreamPlatformInfo(card.platform);
			channel.id = string(info.id) + ":" + card.account.accountId;
			channel.platform = card.platform;
			channel.accountId = card.account.accountId;
		}
		if (channel.displayName.empty()) {
			channel.displayName = card.account.displayName.empty()
						      ? StreamPlatformDisplayName(card.platform).toStdString()
						      : card.account.displayName;
		}

		/* A channel whose ingest credentials are still being resolved would
		 * be rejected by MultiStreamManager::Configure and take every other
		 * channel down with it, so hand it over disabled instead. */
		if (!card.credentialsResolved) {
			channel.server.clear();
			channel.streamKey.clear();
			channel.enabled = false;
		}
		channels.emplace_back(std::move(channel));
	}
	return channels;
}

vector<string> MultistreamAccountsDialog::ManagedChannelIds() const
{
	vector<string> ids;
	for (const auto &card : cards) {
		if (!card.channel.id.empty())
			ids.push_back(card.channel.id);
		else if (!card.account.accountId.empty())
			ids.push_back(string(GetStreamPlatformInfo(card.platform).id) + ":" + card.account.accountId);
	}
	return ids;
}

void MultistreamAccountsDialog::reject()
{
	if (busy) {
		QMessageBox::information(this, windowTitle(), QTStr("Multistream.Accounts.WaitForConnection"));
		return;
	}
	QDialog::reject();
}

void MultistreamAccountsDialog::AddAccountCard(int index, QVBoxLayout *layout)
{
	AccountCard *cardPtr = CardAt(index);
	if (!cardPtr)
		return;
	AccountCard &card = *cardPtr;
	const StreamPlatform platform = card.platform;

	auto *frame = new QFrame(this);
	frame->setObjectName("platformCard");
	frame->setProperty("platform", StreamPlatformId(platform));

	auto *cardLayout = new QVBoxLayout(frame);
	cardLayout->setContentsMargins(18, 16, 18, 16);
	cardLayout->setSpacing(14);

	auto *row = new QHBoxLayout();
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(16);

	/* Same badge the channel bar and the platform picker use, so a platform
	 * looks identical everywhere in the application. */
	auto *iconLabel = new QLabel(frame);
	iconLabel->setObjectName(QStringLiteral("platformIcon"));
	iconLabel->setFixedSize(48, 48);
	iconLabel->setAlignment(Qt::AlignCenter);
	iconLabel->setPixmap(PlatformIconProvider::Badge(platform, 48));
	row->addWidget(iconLabel);

	auto *textLayout = new QVBoxLayout();
	textLayout->setSpacing(6);

	auto *name = new QLabel(StreamPlatformDisplayName(platform), frame);
	name->setObjectName("platformName");

	card.status = new QLabel(QTStr("Multistream.Accounts.NotConnected"), frame);
	card.status->setObjectName("platformStatusPill");
	card.status->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

	auto *statusContainer = new QHBoxLayout();
	statusContainer->setContentsMargins(0, 0, 0, 0);
	statusContainer->addWidget(card.status);
	statusContainer->addStretch();

	textLayout->addWidget(name);
	textLayout->addLayout(statusContainer);
	row->addLayout(textLayout, 1);

	auto *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(8);

	card.connectButton = new QPushButton(QTStr("Multistream.Accounts.Connect"), frame);
	card.connectButton->setObjectName(QStringLiteral("btnConnect_%1").arg(StreamPlatformId(platform)));
	if (platform == StreamPlatform::Kick) {
		card.configureButton = new QPushButton(QTStr("Multistream.Accounts.Configure"), frame);
		card.configureButton->setObjectName(QStringLiteral("btnConfigure_kick"));
		connect(card.configureButton, &QPushButton::clicked, this,
			[this]() { ConfigureKickRegistration(); });
	}
	card.disconnectButton = new QPushButton(QTStr("Multistream.Accounts.Disconnect"), frame);
	card.disconnectButton->setObjectName("btnDisconnect");
	card.disconnectButton->setVisible(false);

	connect(card.connectButton, &QPushButton::clicked, this, [this, index]() { ConnectAccount(index); });
	connect(card.disconnectButton, &QPushButton::clicked, this, [this, index]() { DisconnectAccount(index); });

	if (card.configureButton)
		btnLayout->addWidget(card.configureButton);
	btnLayout->addWidget(card.connectButton);
	btnLayout->addWidget(card.disconnectButton);
	row->addLayout(btnLayout);

	cardLayout->addLayout(row);

	auto *settingsBox = new QFrame(frame);
	settingsBox->setObjectName(QStringLiteral("platformSettings"));

	auto *settingsLayout = new QVBoxLayout(settingsBox);
	settingsLayout->setContentsMargins(4, 4, 4, 4);
	settingsLayout->setSpacing(10);

	auto *settingsTitle = new QLabel(QTStr("Multistream.Accounts.StreamSettings"), settingsBox);
	settingsTitle->setObjectName(QStringLiteral("platformSettingsTitle"));
	settingsLayout->addWidget(settingsTitle);

	auto *audioRow = new QHBoxLayout();
	audioRow->setContentsMargins(0, 0, 0, 0);
	audioRow->setSpacing(12);
	audioRow->addWidget(new QLabel(QTStr("Multistream.Accounts.AudioTrack"), settingsBox));

	card.audioTrackGroup = new QButtonGroup(settingsBox);
	for (int i = 0; i < AUDIO_TRACK_COUNT; i++) {
		/* Button id is the zero-based track index; the label is the track
		 * number the rest of OBS shows. */
		auto *radio = new QRadioButton(QString::number(i + 1), settingsBox);
		card.audioTrackRadios[i] = radio;
		card.audioTrackGroup->addButton(radio, i);
		audioRow->addWidget(radio);
	}
	audioRow->addStretch();
	settingsLayout->addLayout(audioRow);

	auto *vodRow = new QHBoxLayout();
	vodRow->setContentsMargins(0, 0, 0, 0);
	vodRow->setSpacing(12);

	/* Only built where the platform actually honours it: offering the control
	 * elsewhere invites the user to configure something that never takes
	 * effect. The rest of the dialog treats these pointers as optional. */
	if (GetStreamPlatformInfo(platform).supportsVodTrack) {
		card.vodTrackCheckBox = new QCheckBox(QTStr("Multistream.Accounts.VodTrack"), settingsBox);
		vodRow->addWidget(card.vodTrackCheckBox);

		card.vodTrackGroup = new QButtonGroup(settingsBox);
		for (int i = 0; i < AUDIO_TRACK_COUNT; i++) {
			auto *radio = new QRadioButton(QString::number(i + 1), settingsBox);
			radio->setEnabled(false);
			card.vodTrackRadios[i] = radio;
			card.vodTrackGroup->addButton(radio, i);
			vodRow->addWidget(radio);
		}
		vodRow->addStretch();
		settingsLayout->addLayout(vodRow);
	} else {
		delete vodRow;
	}

	cardLayout->addWidget(settingsBox);
	layout->addWidget(frame);

	/* Restore the stored selection for this channel. */
	const size_t audioIndex = card.channel.audioMixIndex < AUDIO_TRACK_COUNT ? card.channel.audioMixIndex : 0;
	if (card.audioTrackRadios[audioIndex])
		card.audioTrackRadios[audioIndex]->setChecked(true);
	if (card.vodTrackCheckBox) {
		card.vodTrackCheckBox->setChecked(card.channel.vodTrackEnabled);
		const size_t vodIndex = card.channel.vodTrackIndex < AUDIO_TRACK_COUNT ? card.channel.vodTrackIndex : 1;
		if (card.vodTrackRadios[vodIndex])
			card.vodTrackRadios[vodIndex]->setChecked(true);
		for (auto *radio : card.vodTrackRadios) {
			if (radio)
				radio->setEnabled(card.channel.vodTrackEnabled);
		}
	}
	UpdateCard(card);

	connect(card.audioTrackGroup, &QButtonGroup::idClicked, this, [this, index](int id) {
		AccountCard *targetCard = CardAt(index);
		if (!targetCard || id < 0 || id >= AUDIO_TRACK_COUNT)
			return;
		targetCard->channel.audioMixIndex = static_cast<size_t>(id);
	});

	if (!card.vodTrackCheckBox)
		return;

	connect(card.vodTrackCheckBox, &QCheckBox::toggled, this, [this, index](bool checked) {
		AccountCard *targetCard = CardAt(index);
		if (!targetCard)
			return;
		targetCard->channel.vodTrackEnabled = checked;
		for (auto *radio : targetCard->vodTrackRadios) {
			if (radio)
				radio->setEnabled(checked);
		}
	});

	connect(card.vodTrackGroup, &QButtonGroup::idClicked, this, [this, index](int id) {
		AccountCard *targetCard = CardAt(index);
		if (!targetCard || id < 0 || id >= AUDIO_TRACK_COUNT)
			return;
		targetCard->channel.vodTrackIndex = static_cast<size_t>(id);
	});
}

void MultistreamAccountsDialog::ResolveChannelInBackground(int index)
{
	AccountCard *card = CardAt(index);
	if (!card || !card->connected || !RegistrationReady(card->platform))
		return;

	QPointer<MultistreamAccountsDialog> guard(this);
	const ConnectedStreamAccount account = card->account;
	const auto registration = RegistrationFor(card->platform);
	const string redirectUri = RedirectUriFor(card->platform);
	MultiStreamChannel pending = card->channel;

	MultistreamTaskPool().start([guard, index, account, registration, redirectUri, pending]() mutable {
		string error;
		const bool resolved =
			ConnectedAccountManager::ResolveChannel(account, registration, redirectUri, pending, error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, index, resolved, resolvedChannel = std::move(pending)]() mutable {
				if (!guard || !resolved)
					return;
				AccountCard *targetCard = guard->CardAt(index);
				if (!targetCard || !targetCard->connected ||
				    targetCard->account.accountId != resolvedChannel.accountId)
					return;
				const size_t audioMixIndex = targetCard->channel.audioMixIndex;
				const bool vodTrackEnabled = targetCard->channel.vodTrackEnabled;
				const size_t vodTrackIndex = targetCard->channel.vodTrackIndex;
				const bool enabled = targetCard->channel.enabled;
				targetCard->channel = std::move(resolvedChannel);
				targetCard->channel.audioMixIndex = audioMixIndex;
				targetCard->channel.vodTrackEnabled = vodTrackEnabled;
				targetCard->channel.vodTrackIndex = vodTrackIndex;
				targetCard->channel.enabled = enabled;
				targetCard->credentialsResolved = !targetCard->channel.server.empty() &&
								  !targetCard->channel.streamKey.empty();
			},
			Qt::QueuedConnection);
	});
}

void MultistreamAccountsDialog::UpdateCard(AccountCard &card)
{
	if (card.connected) {
		const QString name = card.account.displayName.empty() ? StreamPlatformDisplayName(card.platform)
								     : FromStdString(card.account.displayName);
		card.status->setText(QStringLiteral("%1  %2").arg(QChar(0x25cf),
								 QTStr("Multistream.Accounts.ConnectedAs").arg(name)));
		card.status->setProperty("statusState", "connected");
	} else {
		const QString statusMsg = RegistrationStatusText(card.platform);
		if (!RegistrationReady(card.platform)) {
			card.status->setText(QStringLiteral("%1  %2").arg(QChar(0x26a0), statusMsg));
			card.status->setProperty("statusState", "pending");
		} else {
			card.status->setText(QStringLiteral("%1  %2").arg(QChar(0x25cb), statusMsg));
			card.status->setProperty("statusState", "disconnected");
		}
	}

	if (card.status->style()) {
		card.status->style()->unpolish(card.status);
		card.status->style()->polish(card.status);
	}

	card.connectButton->setText(card.connected ? QTStr("Multistream.Accounts.Refresh")
						   : QTStr("Multistream.Accounts.Connect"));
	card.connectButton->setVisible(true);
	card.connectButton->setEnabled(card.platform == StreamPlatform::Kick || RegistrationReady(card.platform));
	card.connectButton->setToolTip(RegistrationToolTip(card.platform));
	if (card.configureButton) {
		card.configureButton->setVisible(true);
		card.configureButton->setEnabled(true);
	}
	card.disconnectButton->setVisible(card.connected);
}

void MultistreamAccountsDialog::ConfigureKickRegistration()
{
	config_t *config = App()->GetUserConfig();
	const string section = StreamPlatformConfigSection(StreamPlatform::Kick);
	auto registration = RegistrationFor(StreamPlatform::Kick);

	bool ok = false;
	const QString inputId =
		QInputDialog::getText(this, QTStr("Multistream.Accounts.KickClientIdTitle"),
				      QTStr("Multistream.Accounts.KickClientIdPrompt")
					      .arg(FromStdString(KickRedirectUri())),
				      QLineEdit::Normal, FromStdString(registration.clientId), &ok);
	if (!ok)
		return;
	if (inputId.trimmed().isEmpty()) {
		QMessageBox::warning(this, QTStr("Multistream.Accounts.ConfigurationRequired"),
				     QTStr("Multistream.Accounts.RegistrationHelp"));
		return;
	}

	config_set_string(config, section.c_str(), "ClientId", inputId.trimmed().toUtf8().constData());
	config_save_safe(config, "tmp", nullptr);
	registration = RegistrationFor(StreamPlatform::Kick);

	/* Only development builds without a token proxy need the secret locally,
	 * and even then it goes to the credential store, never to the .ini. */
	if (registration.tokenExchangeEndpoint.empty()) {
		const QString secret = QInputDialog::getText(this, QTStr("Multistream.Accounts.KickSecretTitle"),
							     QTStr("Multistream.Accounts.KickSecretPrompt"),
							     QLineEdit::Password, QString(), &ok);
		if (ok) {
			if (secret.trimmed().isEmpty()) {
				QMessageBox::warning(this, QTStr("Multistream.Accounts.KickSecretTitle"),
						     QTStr("Multistream.Accounts.KickSecretEmpty"));
			} else {
				string error;
				const QByteArray secretUtf8 = secret.trimmed().toUtf8();
				if (SecureTokenStore::Save(KICK_CLIENT_CREDENTIAL_PLATFORM, registration.clientId,
							   string(secretUtf8.constData(), secretUtf8.size()), error)) {
					QMessageBox::information(this, QTStr("Multistream.Accounts.KickSecretTitle"),
								 QTStr("Multistream.Accounts.KickSecretSaved"));
				} else {
					QMessageBox::critical(
						this, QTStr("Multistream.Accounts.KickSecretTitle"),
						QTStr("Multistream.Accounts.KickSecretSaveFailed")
							.arg(FromStdString(error)));
				}
			}
		}
	}

	if (AccountCard *card = CardForPlatform(StreamPlatform::Kick))
		UpdateCard(*card);
}

void MultistreamAccountsDialog::ConnectAccount(int index)
{
	AccountCard *card = CardAt(index);
	if (!card)
		return;

	const StreamPlatform platform = card->platform;
	OAuthClientRegistration registration = RegistrationFor(platform);
	if (registration.clientId.empty()) {
		if (platform == StreamPlatform::Kick) {
			ConfigureKickRegistration();
			registration = RegistrationFor(platform);
		}
		if (registration.clientId.empty()) {
			QMessageBox::warning(this, QTStr("Multistream.Accounts.ConfigurationRequired"),
					     QTStr("Multistream.Accounts.RegistrationHelp"));
			return;
		}
	}
	if (card->connected) {
		RefreshConnectedAccount(index, registration);
		return;
	}

	if (platform == StreamPlatform::Twitch)
		StartTwitchConnection(index, registration);
	else
		StartPkceConnection(index, registration);
}

void MultistreamAccountsDialog::RefreshConnectedAccount(int index, const OAuthClientRegistration &registration)
{
	AccountCard *card = CardAt(index);
	if (!card)
		return;

	const ConnectedStreamAccount account = card->account;
	const string redirectUri = RedirectUriFor(card->platform);
	SetBusy(index, QTStr("Multistream.Accounts.Refreshing"));
	QPointer<MultistreamAccountsDialog> guard(this);
	MultistreamTaskPool().start([guard, index, account, registration, redirectUri]() {
		ConnectionResult result;
		result.account = account;
		string error;
		result.success =
			ConnectedAccountManager::ResolveChannel(account, registration, redirectUri, result.channel,
								error);
		result.error = FromStdString(error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, index, result = std::move(result)]() mutable {
				if (guard)
					guard->FinishConnection(index, result.success, std::move(result.account),
								std::move(result.channel), result.error);
			},
			Qt::QueuedConnection);
	});
}

void MultistreamAccountsDialog::StartPkceConnection(int index, const OAuthClientRegistration &registration)
{
	AccountCard *card = CardAt(index);
	if (!card)
		return;
	const StreamPlatform platform = card->platform;

	ClearLoopback();
	const quint16 preferredPort = platform == StreamPlatform::Kick ? KickCallbackPort() : 0;
	loopback = new AuthListener(this, preferredPort);
	if (!loopback->IsListening()) {
		ClearLoopback();
		QMessageBox::critical(this, QTStr("Multistream.Accounts.ConnectionFailed"),
				      QTStr("Multistream.Accounts.CallbackUnavailable"));
		return;
	}

	const string redirectUri = QStringLiteral("http://127.0.0.1:%1/").arg(loopback->GetPort()).toStdString();
	OAuthAuthorizationSession session;
	string error;
	if (!PlatformOAuthClient::CreateAuthorizationSession(platform, registration, redirectUri, session, error)) {
		ClearLoopback();
		QMessageBox::critical(this, QTStr("Multistream.Accounts.ConnectionFailed"), FromStdString(error));
		return;
	}

	loopback->SetState(FromStdString(session.state));
	connect(loopback, &AuthListener::fail, this, [this, index]() {
		ClearLoopback();
		FinishConnection(index, false, {}, {}, QTStr("Multistream.Accounts.AuthorizationRejected"));
	});
	connect(loopback, &AuthListener::ok, this,
		[this, index, platform, registration, session](const QString &code) mutable {
			ClearLoopback();
			if (AccountCard *target = CardAt(index))
				target->status->setText(QTStr("Multistream.Accounts.Finishing"));
			QPointer<MultistreamAccountsDialog> guard(this);
			MultistreamTaskPool().start([guard, index, platform, registration, session,
						     code = code.toStdString()]() mutable {
				ConnectionResult result;
				OAuthTokenSet tokens;
				string taskError;
				if (PlatformOAuthClient::ExchangeAuthorizationCode(registration, session, code, tokens,
										   taskError)) {
					result.success = ConnectedAccountManager::CompleteConnection(
						platform, registration, tokens, result.account, result.channel,
						taskError);
				}
				result.error = FromStdString(taskError);
				if (!guard)
					return;
				QMetaObject::invokeMethod(
					guard.data(),
					[guard, index, result = std::move(result)]() mutable {
						if (guard)
							guard->FinishConnection(index, result.success,
										std::move(result.account),
										std::move(result.channel),
										result.error);
					},
					Qt::QueuedConnection);
			});
		});

	SetBusy(index, QTStr("Multistream.Accounts.WaitingForBrowser"));
	if (!QDesktopServices::openUrl(QUrl(FromStdString(session.authorizationUrl)))) {
		ClearLoopback();
		FinishConnection(index, false, {}, {}, QTStr("Multistream.Accounts.BrowserOpenFailed"));
	}
}

void MultistreamAccountsDialog::StartTwitchConnection(int index, const OAuthClientRegistration &registration)
{
	SetBusy(index, QTStr("Multistream.Accounts.RequestingDeviceCode"));
	QPointer<MultistreamAccountsDialog> guard(this);
	MultistreamTaskPool().start([guard, index, registration]() {
		DeviceStartResult result;
		string error;
		result.success = PlatformOAuthClient::StartDeviceAuthorization(StreamPlatform::Twitch, registration,
									      result.authorization, error);
		result.error = FromStdString(error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, index, registration, result = std::move(result)]() mutable {
				if (!guard)
					return;
				if (!result.success) {
					guard->FinishConnection(index, false, {}, {}, result.error);
					return;
				}
				guard->twitchRegistration = registration;
				guard->twitchAuthorization = std::move(result.authorization);
				guard->twitchPollIntervalSeconds = guard->twitchAuthorization.pollIntervalSeconds;
				guard->twitchElapsed.start();
				guard->instructions->setText(
					QTStr("Multistream.Accounts.TwitchDeviceInstructions")
						.arg(FromStdString(guard->twitchAuthorization.verificationUri),
						     FromStdString(guard->twitchAuthorization.userCode)));
				guard->instructions->setVisible(true);
				if (AccountCard *card = guard->CardAt(index))
					card->status->setText(QTStr("Multistream.Accounts.WaitingForApproval"));
				QDesktopServices::openUrl(
					QUrl(FromStdString(guard->twitchAuthorization.verificationUri)));
				guard->twitchPollTimer.start(guard->twitchPollIntervalSeconds * 1000);
			},
			Qt::QueuedConnection);
	});
}

void MultistreamAccountsDialog::PollTwitch()
{
	if (!busy || busyIndex < 0)
		return;
	if (twitchAuthorization.expiresInSeconds > 0 &&
	    twitchElapsed.elapsed() >= static_cast<qint64>(twitchAuthorization.expiresInSeconds) * 1000) {
		FinishConnection(busyIndex, false, {}, {}, QTStr("Multistream.Accounts.DeviceCodeExpired"));
		return;
	}

	const OAuthDeviceAuthorization authorization = twitchAuthorization;
	const OAuthClientRegistration registration = twitchRegistration;
	const int index = busyIndex;
	QPointer<MultistreamAccountsDialog> guard(this);
	MultistreamTaskPool().start([guard, index, authorization, registration]() {
		DevicePollResult result;
		OAuthTokenSet tokens;
		string error;
		result.status = PlatformOAuthClient::PollDeviceAuthorization(StreamPlatform::Twitch, registration,
									     authorization, tokens, error);
		if (result.status == OAuthDevicePollStatus::Authorized) {
			result.connection.success = ConnectedAccountManager::CompleteConnection(
				StreamPlatform::Twitch, registration, tokens, result.connection.account,
				result.connection.channel, error);
			result.connection.error = FromStdString(error);
		}
		result.error = FromStdString(error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(
			guard.data(),
			[guard, index, result = std::move(result)]() mutable {
				if (!guard || !guard->busy)
					return;
				switch (result.status) {
				case OAuthDevicePollStatus::Authorized:
					guard->FinishConnection(index, result.connection.success,
								std::move(result.connection.account),
								std::move(result.connection.channel),
								result.connection.error);
					break;
				case OAuthDevicePollStatus::Pending:
					guard->twitchPollTimer.start(guard->twitchPollIntervalSeconds * 1000);
					break;
				case OAuthDevicePollStatus::SlowDown:
					guard->twitchPollIntervalSeconds += 5;
					guard->twitchPollTimer.start(guard->twitchPollIntervalSeconds * 1000);
					break;
				default:
					guard->FinishConnection(index, false, {}, {}, result.error);
					break;
				}
			},
			Qt::QueuedConnection);
	});
}

void MultistreamAccountsDialog::DisconnectAccount(int index)
{
	AccountCard *card = CardAt(index);
	if (!card || !card->connected)
		return;
	if (QMessageBox::question(this, QTStr("Multistream.Accounts.Disconnect"),
				  QTStr("Multistream.Accounts.ConfirmDisconnect")
					  .arg(StreamPlatformDisplayName(card->platform))) != QMessageBox::Yes)
		return;

	string error;
	if (!ConnectedAccountManager::Disconnect(card->account, error)) {
		QMessageBox::critical(this, QTStr("Multistream.Accounts.ConnectionFailed"), FromStdString(error));
		return;
	}

	/* Kept in the list but marked removed, so the caller knows to drop this
	 * exact channel from the store instead of guessing by platform. */
	card->removed = true;
	card->account = {};
	card->connected = false;
	card->credentialsResolved = false;
	UpdateCard(*card);
}

void MultistreamAccountsDialog::SetBusy(int index, const QString &status)
{
	busy = true;
	busyIndex = index;
	for (auto &card : cards) {
		card.connectButton->setEnabled(false);
		card.disconnectButton->setEnabled(false);
		if (card.configureButton)
			card.configureButton->setEnabled(false);
	}
	closeButton->setEnabled(false);
	cancelButton->setVisible(true);
	cancelButton->setEnabled(true);
	connectionTimeout.start(CONNECTION_TIMEOUT_MS);
	if (AccountCard *card = CardAt(index))
		card->status->setText(status);
}

void MultistreamAccountsDialog::CancelPendingConnection()
{
	if (!busy)
		return;
	ClearLoopback();
	twitchPollTimer.stop();
	/* Reported as a cancellation, not a failure: no message box for something
	 * the user asked for. */
	busy = false;
	busyIndex = -1;
	connectionTimeout.stop();
	instructions->setVisible(false);
	closeButton->setEnabled(true);
	cancelButton->setVisible(false);
	for (auto &card : cards) {
		UpdateCard(card);
		card.disconnectButton->setEnabled(true);
	}
}

void MultistreamAccountsDialog::FinishConnection(int index, bool success, ConnectedStreamAccount account,
						 MultiStreamChannel channel, const QString &error)
{
	busy = false;
	busyIndex = -1;
	twitchPollTimer.stop();
	connectionTimeout.stop();
	instructions->setVisible(false);
	closeButton->setEnabled(true);
	cancelButton->setVisible(false);
	for (auto &card : cards) {
		UpdateCard(card);
		card.disconnectButton->setEnabled(true);
	}

	AccountCard *card = CardAt(index);
	if (!card)
		return;

	if (success) {
		/* Connecting an account that is already in the list would create a
		 * duplicate channel with the same id. */
		const bool duplicate = any_of(cards.begin(), cards.end(), [&](const AccountCard &other) {
			return &other != card && !other.removed && other.connected &&
			       other.platform == card->platform && other.account.accountId == account.accountId;
		});
		if (duplicate) {
			QMessageBox::information(this, QTStr("Multistream.Accounts.Title"),
						 QTStr("Multistream.Accounts.AlreadyConnected")
							 .arg(FromStdString(account.displayName)));
			return;
		}

		const size_t audioMixIndex = card->channel.audioMixIndex;
		const bool vodTrackEnabled = card->channel.vodTrackEnabled;
		const size_t vodTrackIndex = card->channel.vodTrackIndex;
		card->account = std::move(account);
		card->channel = std::move(channel);
		card->channel.audioMixIndex = audioMixIndex;
		card->channel.vodTrackEnabled = vodTrackEnabled;
		card->channel.vodTrackIndex = vodTrackIndex;
		card->channel.enabled = true;
		card->connected = true;
		card->removed = false;
		card->credentialsResolved = !card->channel.server.empty() && !card->channel.streamKey.empty();
		UpdateCard(*card);
		return;
	}

	QMessageBox::critical(this, QTStr("Multistream.Accounts.ConnectionFailed"),
			      error.isEmpty() ? QTStr("Multistream.Accounts.UnknownError") : error);
}

void MultistreamAccountsDialog::ClearLoopback()
{
	if (!loopback)
		return;
	loopback->disconnect(this);
	loopback->deleteLater();
	loopback = nullptr;
}
