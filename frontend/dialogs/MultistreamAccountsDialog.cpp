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

#include <QCheckBox>
#include <QComboBox>
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

QString RegistrationBlockedReason(StreamPlatform platform)
{
	const auto registration = RegistrationFor(platform);
	if (registration.clientId.empty())
		return QTStr("Multistream.Accounts.IntegrationPending");
	if (platform == StreamPlatform::Kick && registration.tokenExchangeEndpoint.empty() &&
	    registration.clientSecret.empty())
		return QTStr("Multistream.Accounts.KickProxyPending");
	return {};
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

QComboBox *MakeTrackCombo(QWidget *parent)
{
	/* Six radio buttons per row was most of the old dialog's height; a combo
	 * says the same thing in one control. */
	auto *combo = new QComboBox(parent);
	combo->setObjectName(QStringLiteral("trackCombo"));
	for (int i = 0; i < AUDIO_TRACK_COUNT; ++i)
		combo->addItem(QString::number(i + 1), i);
	combo->setFixedWidth(58);
	return combo;
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
	setMinimumWidth(660);
	setModal(true);
	/* Styling lives in the theme files (see Yami.obt). A widget stylesheet
	 * here would take precedence over the active theme and break light mode. */
	setObjectName(QStringLiteral("multistreamAccountsDialog"));

	auto *layout = new QVBoxLayout(this);
	/* The device-code instructions appear mid-flow; without this the dialog
	 * keeps its original height and the rows overlap. */
	layout->setSizeConstraint(QLayout::SetMinimumSize);
	layout->setSpacing(10);

	auto *subtitle = new QLabel(QTStr("Multistream.Accounts.Subtitle"), this);
	subtitle->setObjectName(QStringLiteral("infoBanner"));
	subtitle->setWordWrap(true);
	layout->addWidget(subtitle);

	/* One row per connected account, so a platform can hold several. */
	for (auto &channel : existingChannels) {
		if (GetStreamPlatformInfo(channel.platform).ingestMode != StreamIngestMode::ResolvedByApi)
			continue;
		if (newAccountPlatform && channel.platform != *newAccountPlatform)
			continue;

		auto row = make_unique<AccountRow>();
		row->platform = channel.platform;
		row->account = {channel.platform, channel.accountId, channel.displayName, channel.enabled};
		row->connected = !channel.accountId.empty();
		row->credentialsResolved = !channel.server.empty() && !channel.streamKey.empty();
		row->channel = std::move(channel);
		rows.push_back(std::move(row));
	}

	/* Focused on one platform, only that group is shown; otherwise every
	 * platform gets a group so a first account can be added from here. */
	vector<StreamPlatform> platforms;
	if (newAccountPlatform) {
		platforms.push_back(*newAccountPlatform);
	} else {
		for (const auto platform : {StreamPlatform::YouTube, StreamPlatform::Twitch, StreamPlatform::Kick})
			platforms.push_back(platform);
	}

	for (const auto platform : platforms) {
		vector<int> rowIndexes;
		for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
			if (rows[static_cast<size_t>(index)]->platform == platform)
				rowIndexes.push_back(index);
		}
		BuildPlatformGroup(platform, rowIndexes, layout);
	}

	instructions = new QLabel(this);
	instructions->setObjectName(QStringLiteral("accountInstructions"));
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

	for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
		if (rows[static_cast<size_t>(index)]->connected &&
		    !rows[static_cast<size_t>(index)]->credentialsResolved)
			ResolveChannelInBackground(index);
	}
}

MultistreamAccountsDialog::AccountRow *MultistreamAccountsDialog::RowAt(int index)
{
	if (index < 0 || index >= static_cast<int>(rows.size()))
		return nullptr;
	return rows[static_cast<size_t>(index)].get();
}

void MultistreamAccountsDialog::BuildPlatformGroup(StreamPlatform platform, const vector<int> &rowIndexes,
						   QVBoxLayout *layout)
{
	auto *frame = new QFrame(this);
	frame->setObjectName(QStringLiteral("platformGroup"));
	frame->setProperty("platform", StreamPlatformId(platform));

	auto *groupLayout = new QVBoxLayout(frame);
	groupLayout->setContentsMargins(14, 12, 14, 12);
	groupLayout->setSpacing(8);

	auto *header = new QHBoxLayout();
	header->setContentsMargins(0, 0, 0, 0);
	header->setSpacing(10);

	auto *icon = new QLabel(frame);
	icon->setObjectName(QStringLiteral("platformIcon"));
	icon->setFixedSize(28, 28);
	icon->setPixmap(PlatformIconProvider::Badge(platform, 28));
	header->addWidget(icon);

	auto *name = new QLabel(StreamPlatformDisplayName(platform), frame);
	name->setObjectName(QStringLiteral("platformName"));
	header->addWidget(name);
	header->addStretch(1);

	if (platform == StreamPlatform::Kick) {
		auto *configure = new QPushButton(QTStr("Multistream.Accounts.Configure"), frame);
		configure->setObjectName(QStringLiteral("btnConfigure_kick"));
		connect(configure, &QPushButton::clicked, this, [this]() { ConfigureKickRegistration(); });
		header->addWidget(configure);
	}

	auto *addButton = new QPushButton(QTStr("Multistream.Accounts.AddAccount"), frame);
	addButton->setObjectName(QStringLiteral("btnAddAccount"));
	addButton->setEnabled(platform == StreamPlatform::Kick || RegistrationReady(platform));
	addButton->setToolTip(RegistrationToolTip(platform));
	connect(addButton, &QPushButton::clicked, this, [this, platform]() {
		const int index = AddPendingAccount(platform);
		if (index >= 0)
			ConnectAccount(index);
	});
	header->addWidget(addButton);

	groupLayout->addLayout(header);

	/* Rows live in their own layout so a new account can be appended without
	 * rebuilding the group. */
	auto *list = new QVBoxLayout();
	list->setContentsMargins(0, 0, 0, 0);
	list->setSpacing(6);
	groupLayout->addLayout(list);
	groupLayouts.insert(static_cast<int>(platform), list);

	auto *hint = new QLabel(frame);
	hint->setObjectName(QStringLiteral("groupHint"));
	hint->setWordWrap(true);
	groupLayout->addWidget(hint);
	groupHints.insert(static_cast<int>(platform), hint);

	for (const int index : rowIndexes)
		list->addWidget(BuildAccountRow(index));

	UpdateGroupHint(platform);
	layout->addWidget(frame);
}

QWidget *MultistreamAccountsDialog::BuildAccountRow(int index)
{
	AccountRow *rowPtr = RowAt(index);
	if (!rowPtr)
		return nullptr;
	AccountRow &row = *rowPtr;

	auto *widget = new QFrame(this);
	widget->setObjectName(QStringLiteral("accountRow"));
	row.widget = widget;

	auto *outer = new QVBoxLayout(widget);
	outer->setContentsMargins(10, 8, 10, 8);
	outer->setSpacing(6);

	auto *top = new QHBoxLayout();
	top->setContentsMargins(0, 0, 0, 0);
	top->setSpacing(10);

	auto *names = new QVBoxLayout();
	names->setContentsMargins(0, 0, 0, 0);
	names->setSpacing(0);

	row.nameLabel = new QLabel(widget);
	row.nameLabel->setObjectName(QStringLiteral("accountName"));
	row.statusLabel = new QLabel(widget);
	row.statusLabel->setObjectName(QStringLiteral("accountStatus"));

	names->addWidget(row.nameLabel);
	names->addWidget(row.statusLabel);
	top->addLayout(names, 1);

	row.connectButton = new QPushButton(widget);
	row.connectButton->setObjectName(QStringLiteral("btnConnect_%1").arg(StreamPlatformId(row.platform)));
	connect(row.connectButton, &QPushButton::clicked, this, [this, index]() { ConnectAccount(index); });
	top->addWidget(row.connectButton);

	row.disconnectButton = new QPushButton(QTStr("Multistream.Accounts.Disconnect"), widget);
	row.disconnectButton->setObjectName(QStringLiteral("btnDisconnect"));
	connect(row.disconnectButton, &QPushButton::clicked, this, [this, index]() { DisconnectAccount(index); });
	top->addWidget(row.disconnectButton);

	outer->addLayout(top);

	auto *settings = new QHBoxLayout();
	settings->setContentsMargins(0, 0, 0, 0);
	settings->setSpacing(8);

	auto *audioLabel = new QLabel(QTStr("Multistream.Accounts.AudioTrack"), widget);
	audioLabel->setObjectName(QStringLiteral("trackLabel"));
	settings->addWidget(audioLabel);

	row.audioTrackCombo = MakeTrackCombo(widget);
	const int audioIndex = static_cast<int>(row.channel.audioMixIndex);
	row.audioTrackCombo->setCurrentIndex(audioIndex >= 0 && audioIndex < AUDIO_TRACK_COUNT ? audioIndex : 0);
	connect(row.audioTrackCombo, &QComboBox::currentIndexChanged, this, [this, index](int value) {
		if (AccountRow *target = RowAt(index); target && value >= 0)
			target->channel.audioMixIndex = static_cast<size_t>(value);
	});
	settings->addWidget(row.audioTrackCombo);

	/* Only built where the platform honours it: offering the control elsewhere
	 * invites the user to configure something that never takes effect. */
	if (GetStreamPlatformInfo(row.platform).supportsVodTrack) {
		settings->addSpacing(10);

		row.vodTrackCheckBox = new QCheckBox(QTStr("Multistream.Accounts.VodTrack"), widget);
		row.vodTrackCheckBox->setChecked(row.channel.vodTrackEnabled);
		settings->addWidget(row.vodTrackCheckBox);

		row.vodTrackCombo = MakeTrackCombo(widget);
		const int vodIndex = static_cast<int>(row.channel.vodTrackIndex);
		row.vodTrackCombo->setCurrentIndex(vodIndex >= 0 && vodIndex < AUDIO_TRACK_COUNT ? vodIndex : 1);
		row.vodTrackCombo->setEnabled(row.channel.vodTrackEnabled);
		settings->addWidget(row.vodTrackCombo);

		connect(row.vodTrackCheckBox, &QCheckBox::toggled, this, [this, index](bool checked) {
			AccountRow *target = RowAt(index);
			if (!target)
				return;
			target->channel.vodTrackEnabled = checked;
			if (target->vodTrackCombo)
				target->vodTrackCombo->setEnabled(checked);
		});
		connect(row.vodTrackCombo, &QComboBox::currentIndexChanged, this, [this, index](int value) {
			if (AccountRow *target = RowAt(index); target && value >= 0)
				target->channel.vodTrackIndex = static_cast<size_t>(value);
		});
	}

	settings->addStretch(1);
	outer->addLayout(settings);

	UpdateRow(row);
	return widget;
}

int MultistreamAccountsDialog::AddPendingAccount(StreamPlatform platform)
{
	auto *list = groupLayouts.value(static_cast<int>(platform), nullptr);
	if (!list)
		return -1;

	/* Reuse a row that is already waiting to be connected instead of stacking
	 * empty ones every time the button is pressed. */
	for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
		AccountRow *existing = rows[static_cast<size_t>(index)].get();
		if (existing->platform == platform && !existing->connected && !existing->removed)
			return index;
	}

	auto row = make_unique<AccountRow>();
	row->platform = platform;
	row->channel.platform = platform;
	rows.push_back(std::move(row));

	const int index = static_cast<int>(rows.size()) - 1;
	if (QWidget *widget = BuildAccountRow(index))
		list->addWidget(widget);
	UpdateGroupHint(platform);
	return index;
}

void MultistreamAccountsDialog::UpdateRow(AccountRow &row)
{
	const QString platformName = StreamPlatformDisplayName(row.platform);

	if (row.connected) {
		row.nameLabel->setText(row.account.displayName.empty() ? platformName
								       : FromStdString(row.account.displayName));
		row.statusLabel->setText(QStringLiteral("%1  %2").arg(QChar(0x25cf),
								     QTStr("Multistream.Accounts.Connected")));
		row.statusLabel->setProperty("statusState", "connected");
	} else {
		const QString blocked = RegistrationBlockedReason(row.platform);
		row.nameLabel->setText(QTStr("Multistream.Accounts.NewAccount"));
		row.statusLabel->setText(blocked.isEmpty()
						 ? QStringLiteral("%1  %2").arg(QChar(0x25cb),
										QTStr("Multistream.Accounts.NotConnected"))
						 : QStringLiteral("%1  %2").arg(QChar(0x26a0), blocked));
		row.statusLabel->setProperty("statusState", blocked.isEmpty() ? "disconnected" : "pending");
	}

	if (row.statusLabel->style()) {
		row.statusLabel->style()->unpolish(row.statusLabel);
		row.statusLabel->style()->polish(row.statusLabel);
	}

	row.connectButton->setText(row.connected ? QTStr("Multistream.Accounts.Refresh")
						 : QTStr("Multistream.Accounts.Connect"));
	row.connectButton->setEnabled(row.platform == StreamPlatform::Kick || RegistrationReady(row.platform));
	row.connectButton->setToolTip(RegistrationToolTip(row.platform));
	row.disconnectButton->setVisible(row.connected);

	/* Track selection only means something once the account exists. */
	row.audioTrackCombo->setEnabled(row.connected);
	if (row.vodTrackCheckBox)
		row.vodTrackCheckBox->setEnabled(row.connected);
	if (row.vodTrackCombo)
		row.vodTrackCombo->setEnabled(row.connected && row.channel.vodTrackEnabled);
}

void MultistreamAccountsDialog::UpdateGroupHint(StreamPlatform platform)
{
	QLabel *hint = groupHints.value(static_cast<int>(platform), nullptr);
	if (!hint)
		return;

	const bool hasRow = any_of(rows.begin(), rows.end(), [platform](const unique_ptr<AccountRow> &row) {
		return row->platform == platform && row->widget;
	});
	hint->setText(QTStr("Multistream.Accounts.GroupEmpty"));
	hint->setVisible(!hasRow);
}

void MultistreamAccountsDialog::ResolveChannelInBackground(int index)
{
	AccountRow *row = RowAt(index);
	if (!row || !row->connected || !RegistrationReady(row->platform))
		return;

	QPointer<MultistreamAccountsDialog> guard(this);
	const ConnectedStreamAccount account = row->account;
	const auto registration = RegistrationFor(row->platform);
	const string redirectUri = RedirectUriFor(row->platform);
	MultiStreamChannel pending = row->channel;

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
				AccountRow *target = guard->RowAt(index);
				if (!target || !target->connected ||
				    target->account.accountId != resolvedChannel.accountId)
					return;
				/* Keep what the user chose here; take only what the
				 * platform resolved. */
				const size_t audioMixIndex = target->channel.audioMixIndex;
				const bool vodTrackEnabled = target->channel.vodTrackEnabled;
				const size_t vodTrackIndex = target->channel.vodTrackIndex;
				const bool enabled = target->channel.enabled;
				target->channel = std::move(resolvedChannel);
				target->channel.audioMixIndex = audioMixIndex;
				target->channel.vodTrackEnabled = vodTrackEnabled;
				target->channel.vodTrackIndex = vodTrackIndex;
				target->channel.enabled = enabled;
				target->credentialsResolved =
					!target->channel.server.empty() && !target->channel.streamKey.empty();
			},
			Qt::QueuedConnection);
	});
}

vector<MultiStreamChannel> MultistreamAccountsDialog::Channels() const
{
	vector<MultiStreamChannel> channels;
	for (const auto &row : rows) {
		if (row->removed || !row->connected || row->account.accountId.empty())
			continue;

		MultiStreamChannel channel = row->channel;
		if (channel.id.empty()) {
			const auto &info = GetStreamPlatformInfo(row->platform);
			channel.id = string(info.id) + ":" + row->account.accountId;
			channel.platform = row->platform;
			channel.accountId = row->account.accountId;
		}
		if (channel.displayName.empty()) {
			channel.displayName = row->account.displayName.empty()
						      ? StreamPlatformDisplayName(row->platform).toStdString()
						      : row->account.displayName;
		}

		/* A channel whose ingest credentials are still being resolved would
		 * be rejected by MultiStreamManager::Configure and take every other
		 * channel down with it, so hand it over disabled instead. */
		if (!row->credentialsResolved) {
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
	for (const auto &row : rows) {
		if (!row->channel.id.empty())
			ids.push_back(row->channel.id);
		else if (!row->account.accountId.empty())
			ids.push_back(string(GetStreamPlatformInfo(row->platform).id) + ":" + row->account.accountId);
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

	for (auto &row : rows) {
		if (row->platform == StreamPlatform::Kick)
			UpdateRow(*row);
	}
}

void MultistreamAccountsDialog::ConnectAccount(int index)
{
	AccountRow *row = RowAt(index);
	if (!row)
		return;

	const StreamPlatform platform = row->platform;
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
	if (row->connected) {
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
	AccountRow *row = RowAt(index);
	if (!row)
		return;

	const ConnectedStreamAccount account = row->account;
	const string redirectUri = RedirectUriFor(row->platform);
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
	AccountRow *row = RowAt(index);
	if (!row)
		return;
	const StreamPlatform platform = row->platform;

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
			if (AccountRow *target = RowAt(index))
				target->statusLabel->setText(QTStr("Multistream.Accounts.Finishing"));
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
				if (AccountRow *row = guard->RowAt(index))
					row->statusLabel->setText(QTStr("Multistream.Accounts.WaitingForApproval"));
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
	AccountRow *row = RowAt(index);
	if (!row || !row->connected)
		return;

	const QString name = row->account.displayName.empty() ? StreamPlatformDisplayName(row->platform)
							      : FromStdString(row->account.displayName);
	if (QMessageBox::question(this, QTStr("Multistream.Accounts.Disconnect"),
				  QTStr("Multistream.Accounts.ConfirmDisconnect").arg(name)) != QMessageBox::Yes)
		return;

	string error;
	if (!ConnectedAccountManager::Disconnect(row->account, error)) {
		QMessageBox::critical(this, QTStr("Multistream.Accounts.ConnectionFailed"), FromStdString(error));
		return;
	}

	/* The row disappears from the dialog and, because its id stays in
	 * ManagedChannelIds(), the caller drops exactly this channel. */
	row->removed = true;
	row->connected = false;
	row->credentialsResolved = false;
	row->account = {};
	if (row->widget) {
		row->widget->hide();
		row->widget->deleteLater();
		row->widget = nullptr;
	}
	UpdateGroupHint(row->platform);
}

void MultistreamAccountsDialog::SetControlsEnabled(bool enabled)
{
	for (auto &row : rows) {
		if (!row->widget)
			continue;
		row->connectButton->setEnabled(enabled && (row->platform == StreamPlatform::Kick ||
							   RegistrationReady(row->platform)));
		row->disconnectButton->setEnabled(enabled);
	}
	for (auto *button : findChildren<QPushButton *>(QStringLiteral("btnAddAccount")))
		button->setEnabled(enabled);
	for (auto *button : findChildren<QPushButton *>(QStringLiteral("btnConfigure_kick")))
		button->setEnabled(enabled);
}

void MultistreamAccountsDialog::SetBusy(int index, const QString &status)
{
	busy = true;
	busyIndex = index;
	SetControlsEnabled(false);
	closeButton->setEnabled(false);
	cancelButton->setVisible(true);
	cancelButton->setEnabled(true);
	connectionTimeout.start(CONNECTION_TIMEOUT_MS);
	if (AccountRow *row = RowAt(index))
		row->statusLabel->setText(status);
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
	SetControlsEnabled(true);
	for (auto &row : rows) {
		if (row->widget)
			UpdateRow(*row);
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
	SetControlsEnabled(true);

	AccountRow *row = RowAt(index);
	if (!row) {
		for (auto &item : rows) {
			if (item->widget)
				UpdateRow(*item);
		}
		return;
	}

	if (success) {
		/* Connecting an account that is already listed would create a second
		 * channel with the same id. */
		const bool duplicate = any_of(rows.begin(), rows.end(), [&](const unique_ptr<AccountRow> &other) {
			return other.get() != row && !other->removed && other->connected &&
			       other->platform == row->platform && other->account.accountId == account.accountId;
		});
		if (duplicate) {
			QMessageBox::information(this, windowTitle(),
						 QTStr("Multistream.Accounts.AlreadyConnected")
							 .arg(FromStdString(account.displayName)));
			for (auto &item : rows) {
				if (item->widget)
					UpdateRow(*item);
			}
			return;
		}

		const size_t audioMixIndex = row->channel.audioMixIndex;
		const bool vodTrackEnabled = row->channel.vodTrackEnabled;
		const size_t vodTrackIndex = row->channel.vodTrackIndex;
		row->account = std::move(account);
		row->channel = std::move(channel);
		row->channel.audioMixIndex = audioMixIndex;
		row->channel.vodTrackEnabled = vodTrackEnabled;
		row->channel.vodTrackIndex = vodTrackIndex;
		row->channel.enabled = true;
		row->connected = true;
		row->removed = false;
		row->credentialsResolved = !row->channel.server.empty() && !row->channel.streamKey.empty();
	}

	for (auto &item : rows) {
		if (item->widget)
			UpdateRow(*item);
	}

	if (!success) {
		QMessageBox::critical(this, QTStr("Multistream.Accounts.ConnectionFailed"),
				      error.isEmpty() ? QTStr("Multistream.Accounts.UnknownError") : error);
	}
}

void MultistreamAccountsDialog::ClearLoopback()
{
	if (!loopback)
		return;
	loopback->disconnect(this);
	loopback->deleteLater();
	loopback = nullptr;
}
