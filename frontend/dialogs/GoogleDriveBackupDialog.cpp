/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "GoogleDriveBackupDialog.hpp"

#include <oauth/AuthListener.hpp>
#include <oauth/PlatformOAuthClient.hpp>
#include <utility/CloudBackupArchive.hpp>
#include <utility/MultistreamTaskPool.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <util/config-file.h>
#include <util/base.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

#include "moc_GoogleDriveBackupDialog.cpp"

using namespace std;

namespace {
constexpr const char *CONFIG_SECTION = "GoogleDriveBackup";

QString HumanSize(qint64 bytes)
{
	if (bytes >= 1024LL * 1024)
		return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
	if (bytes >= 1024)
		return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
	return QStringLiteral("%1 B").arg(bytes);
}

class RestoreOptionsDialog : public QDialog {
public:
	RestoreOptionsDialog(QWidget *parent, bool encryptedBackup) : QDialog(parent), encrypted(encryptedBackup)
	{
		setWindowTitle(QTStr("CloudBackup.Restore.Title"));
		setMinimumWidth(540);
		auto *layout = new QVBoxLayout(this);
		auto *intro = new QLabel(QTStr("CloudBackup.Restore.Explanation"), this);
		intro->setWordWrap(true);
		layout->addWidget(intro);
		general = new QCheckBox(QTStr("CloudBackup.Section.General"), this);
		profiles = new QCheckBox(QTStr("CloudBackup.Section.Profiles"), this);
		scenes = new QCheckBox(QTStr("CloudBackup.Section.Scenes"), this);
		plugins = new QCheckBox(QTStr("CloudBackup.Section.Plugins"), this);
		for (QCheckBox *check : {general, profiles, scenes, plugins}) {
			check->setChecked(true);
			layout->addWidget(check);
		}
		preserveDevices = new QCheckBox(QTStr("CloudBackup.Restore.PreserveDevices"), this);
		preserveDevices->setChecked(true);
		preserveDevices->setToolTip(QTStr("CloudBackup.Restore.PreserveDevices.Help"));
		layout->addWidget(preserveDevices);
		if (encryptedBackup) {
			passphrase = new QLineEdit(this);
			passphrase->setEchoMode(QLineEdit::Password);
			passphrase->setPlaceholderText(QTStr("CloudBackup.Passphrase.Placeholder"));
			layout->addWidget(passphrase);
			restoreCredentials = new QCheckBox(QTStr("CloudBackup.Restore.Credentials"), this);
			restoreCredentials->setChecked(true);
			restoreCredentials->setToolTip(QTStr("CloudBackup.Restore.Credentials.Help"));
			layout->addWidget(restoreCredentials);
		}
		auto *mappingLabel = new QLabel(QTStr("CloudBackup.Restore.PathMappings"), this);
		mappingLabel->setWordWrap(true);
		layout->addWidget(mappingLabel);
		mappings = new QPlainTextEdit(this);
		mappings->setPlaceholderText(QStringLiteral("D:\\Midia => E:\\Midia\nC:\\Users\\Antigo => C:\\Users\\Novo"));
		mappings->setMaximumHeight(95);
		layout->addWidget(mappings);
		auto *warning = new QLabel(QTStr("CloudBackup.Restore.RestartWarning"), this);
		warning->setWordWrap(true);
		warning->setProperty("class", "warning");
		layout->addWidget(warning);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
		buttons->button(QDialogButtonBox::Ok)->setText(QTStr("CloudBackup.Restore.Action"));
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);
	}

	CloudBackupOptions Options() const
	{
		CloudBackupOptions options;
		options.generalSettings = general->isChecked();
		options.profiles = profiles->isChecked();
		options.scenes = scenes->isChecked();
		options.pluginSettings = plugins->isChecked();
		options.restoreCredentials = encrypted && restoreCredentials && restoreCredentials->isChecked();
		options.preserveLocalDevices = preserveDevices->isChecked();
		options.passphrase = passphrase ? passphrase->text() : QString{};
		for (const QString &line : mappings->toPlainText().split('\n', Qt::SkipEmptyParts)) {
			const qsizetype separator = line.indexOf(QStringLiteral("=>"));
			if (separator > 0) {
				const QString from = line.left(separator).trimmed();
				const QString to = line.mid(separator + 2).trimmed();
				if (!from.isEmpty() && !to.isEmpty())
					options.pathMappings.push_back({from, to});
			}
		}
		return options;
	}

private:
	QCheckBox *general = nullptr;
	QCheckBox *profiles = nullptr;
	QCheckBox *scenes = nullptr;
	QCheckBox *plugins = nullptr;
	QCheckBox *preserveDevices = nullptr;
	QLineEdit *passphrase = nullptr;
	QCheckBox *restoreCredentials = nullptr;
	QPlainTextEdit *mappings = nullptr;
	bool encrypted = false;
};
} // namespace

GoogleDriveBackupDialog::GoogleDriveBackupDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QTStr("CloudBackup.Title"));
	setMinimumSize(790, 650);
	setModal(true);
	BuildUi();
	LoadSettings();
	UpdateConnectionUi();
	if (GoogleDriveBackupClient::HasConnection())
		RefreshHistory();
}

GoogleDriveBackupDialog::~GoogleDriveBackupDialog()
{
	ClearLoopback();
}

void GoogleDriveBackupDialog::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(20, 18, 20, 18);
	root->setSpacing(14);

	auto *header = new QFrame(this);
	header->setProperty("class", "settings-group");
	auto *headerLayout = new QHBoxLayout(header);
	auto *icon = new QLabel(QStringLiteral("☁"), header);
	icon->setStyleSheet(QStringLiteral("font-size: 28px; color: #4285f4;"));
	headerLayout->addWidget(icon);
	auto *headerText = new QVBoxLayout;
	auto *title = new QLabel(QTStr("CloudBackup.Header"), header);
	title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
	auto *subtitle = new QLabel(QTStr("CloudBackup.Subtitle"), header);
	subtitle->setWordWrap(true);
	headerText->addWidget(title);
	headerText->addWidget(subtitle);
	headerLayout->addLayout(headerText, 1);
	root->addWidget(header);

	auto *account = new QGroupBox(QTStr("CloudBackup.Account"), this);
	auto *accountLayout = new QHBoxLayout(account);
	accountStatus = new QLabel(account);
	accountStatus->setWordWrap(true);
	accountLayout->addWidget(accountStatus, 1);
	configureButton = new QPushButton(QTStr("CloudBackup.ConfigureClient"), account);
	connectButton = new QPushButton(account);
	accountLayout->addWidget(configureButton);
	accountLayout->addWidget(connectButton);
	connect(configureButton, &QPushButton::clicked, this, &GoogleDriveBackupDialog::ConfigureClientId);
	connect(connectButton, &QPushButton::clicked, this, [this]() {
		GoogleDriveBackupClient::HasConnection() ? DisconnectGoogle() : ConnectGoogle();
	});
	root->addWidget(account);

	auto *content = new QHBoxLayout;
	content->setSpacing(14);
	auto *optionsGroup = new QGroupBox(QTStr("CloudBackup.WhatToSave"), this);
	auto *optionsLayout = new QVBoxLayout(optionsGroup);
	generalCheck = new QCheckBox(QTStr("CloudBackup.Section.General"), optionsGroup);
	profilesCheck = new QCheckBox(QTStr("CloudBackup.Section.Profiles"), optionsGroup);
	scenesCheck = new QCheckBox(QTStr("CloudBackup.Section.Scenes"), optionsGroup);
	pluginsCheck = new QCheckBox(QTStr("CloudBackup.Section.Plugins"), optionsGroup);
	for (QCheckBox *check : {generalCheck, profilesCheck, scenesCheck, pluginsCheck})
		optionsLayout->addWidget(check);
	secretsCheck = new QCheckBox(QTStr("CloudBackup.IncludeSecrets"), optionsGroup);
	secretsCheck->setEnabled(CloudBackupArchive::EncryptionSupported());
	secretsCheck->setToolTip(QTStr("CloudBackup.IncludeSecrets.Help"));
	optionsLayout->addWidget(secretsCheck);
	passphraseEdit = new QLineEdit(optionsGroup);
	passphraseEdit->setEchoMode(QLineEdit::Password);
	passphraseEdit->setPlaceholderText(QTStr("CloudBackup.Passphrase.Placeholder"));
	passphraseEdit->setVisible(false);
	optionsLayout->addWidget(passphraseEdit);
	connect(secretsCheck, &QCheckBox::toggled, passphraseEdit, &QWidget::setVisible);
	optionsLayout->addStretch();
	content->addWidget(optionsGroup, 1);

	auto *autoGroup = new QGroupBox(QTStr("CloudBackup.Automatic"), this);
	auto *autoLayout = new QFormLayout(autoGroup);
	autoBackupCheck = new QCheckBox(QTStr("CloudBackup.Automatic.Enable"), autoGroup);
	intervalCombo = new QComboBox(autoGroup);
	intervalCombo->addItem(QTStr("CloudBackup.Interval.Daily"), 1);
	intervalCombo->addItem(QTStr("CloudBackup.Interval.Weekly"), 7);
	retentionCombo = new QComboBox(autoGroup);
	for (int count : {5, 10, 20, 50})
		retentionCombo->addItem(QTStr("CloudBackup.KeepCount").arg(count), count);
	autoLayout->addRow(autoBackupCheck);
	autoLayout->addRow(QTStr("CloudBackup.Interval"), intervalCombo);
	autoLayout->addRow(QTStr("CloudBackup.Retention"), retentionCombo);
	auto *autoHint = new QLabel(QTStr("CloudBackup.Automatic.Help"), autoGroup);
	autoHint->setWordWrap(true);
	autoLayout->addRow(autoHint);
	content->addWidget(autoGroup, 1);
	root->addLayout(content);

	auto *historyGroup = new QGroupBox(QTStr("CloudBackup.History"), this);
	auto *historyLayout = new QVBoxLayout(historyGroup);
	history = new QTreeWidget(historyGroup);
	history->setRootIsDecorated(false);
	history->setAlternatingRowColors(true);
	history->setSelectionMode(QAbstractItemView::SingleSelection);
	history->setHeaderLabels({QTStr("CloudBackup.Column.Date"), QTStr("CloudBackup.Column.Computer"),
				  QTStr("CloudBackup.Column.Version"), QTStr("CloudBackup.Column.Size"),
				  QTStr("CloudBackup.Column.Security")});
	history->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	history->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	history->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	history->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	history->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
	historyLayout->addWidget(history);
	auto *historyButtons = new QHBoxLayout;
	refreshButton = new QPushButton(QTStr("CloudBackup.Refresh"), historyGroup);
	restoreButton = new QPushButton(QTStr("CloudBackup.Restore"), historyGroup);
	exportButton = new QPushButton(QTStr("CloudBackup.Export"), historyGroup);
	deleteButton = new QPushButton(QTStr("CloudBackup.Delete"), historyGroup);
	importButton = new QPushButton(QTStr("CloudBackup.Import"), historyGroup);
	historyButtons->addWidget(refreshButton);
	historyButtons->addWidget(restoreButton);
	historyButtons->addWidget(exportButton);
	historyButtons->addWidget(deleteButton);
	historyButtons->addStretch();
	historyButtons->addWidget(importButton);
	historyLayout->addLayout(historyButtons);
	root->addWidget(historyGroup, 1);

	connect(refreshButton, &QPushButton::clicked, this, &GoogleDriveBackupDialog::RefreshHistory);
	connect(restoreButton, &QPushButton::clicked, this, &GoogleDriveBackupDialog::RestoreSelected);
	connect(exportButton, &QPushButton::clicked, this, &GoogleDriveBackupDialog::ExportSelected);
	connect(deleteButton, &QPushButton::clicked, this, &GoogleDriveBackupDialog::DeleteSelected);
	connect(importButton, &QPushButton::clicked, this, &GoogleDriveBackupDialog::ImportLocal);
	connect(history, &QTreeWidget::itemDoubleClicked, this,
		[this](QTreeWidgetItem *, int) { RestoreSelected(); });
	connect(history, &QTreeWidget::itemSelectionChanged, this, &GoogleDriveBackupDialog::UpdateConnectionUi);

	auto *footer = new QHBoxLayout;
	activityStatus = new QLabel(QTStr("CloudBackup.Ready"), this);
	activityStatus->setWordWrap(true);
	footer->addWidget(activityStatus, 1);
	backupButton = new QPushButton(QTStr("CloudBackup.BackupNow"), this);
	backupButton->setProperty("class", "primary");
	auto *closeButton = new QPushButton(QTStr("Close"), this);
	footer->addWidget(backupButton);
	footer->addWidget(closeButton);
	root->addLayout(footer);
	connect(backupButton, &QPushButton::clicked, this, &GoogleDriveBackupDialog::BackupNow);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void GoogleDriveBackupDialog::LoadSettings()
{
	config_t *config = App()->GetUserConfig();
	auto storedBool = [&](const char *name, bool fallback) {
		return config_has_user_value(config, CONFIG_SECTION, name) ? config_get_bool(config, CONFIG_SECTION, name)
									 : fallback;
	};
	generalCheck->setChecked(storedBool("IncludeGeneral", true));
	profilesCheck->setChecked(storedBool("IncludeProfiles", true));
	scenesCheck->setChecked(storedBool("IncludeScenes", true));
	pluginsCheck->setChecked(storedBool("IncludePlugins", true));
	autoBackupCheck->setChecked(storedBool("AutoEnabled", false));
	const int days = static_cast<int>(config_get_int(config, CONFIG_SECTION, "IntervalDays"));
	intervalCombo->setCurrentIndex(max(0, intervalCombo->findData(days > 0 ? days : 7)));
	const int retain = static_cast<int>(config_get_int(config, CONFIG_SECTION, "Retention"));
	retentionCombo->setCurrentIndex(max(0, retentionCombo->findData(retain > 0 ? retain : 10)));
}

void GoogleDriveBackupDialog::SaveSettings()
{
	config_t *config = App()->GetUserConfig();
	config_set_bool(config, CONFIG_SECTION, "IncludeGeneral", generalCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "IncludeProfiles", profilesCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "IncludeScenes", scenesCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "IncludePlugins", pluginsCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "AutoEnabled", autoBackupCheck->isChecked());
	config_set_int(config, CONFIG_SECTION, "IntervalDays", intervalCombo->currentData().toInt());
	config_set_int(config, CONFIG_SECTION, "Retention", retentionCombo->currentData().toInt());
	config_save_safe(config, "tmp", nullptr);
}

void GoogleDriveBackupDialog::UpdateConnectionUi()
{
	const bool connected = GoogleDriveBackupClient::HasConnection();
	const char *stored = config_get_string(App()->GetUserConfig(), CONFIG_SECTION, "AccountName");
	accountStatus->setText(connected ? QTStr("CloudBackup.ConnectedAs").arg(stored && *stored ? QString::fromUtf8(stored)
											 : QTStr("CloudBackup.GoogleAccount"))
					 : QTStr("CloudBackup.Disconnected"));
	connectButton->setText(connected ? QTStr("CloudBackup.Disconnect") : QTStr("CloudBackup.Connect"));
	connectButton->setEnabled(!busy);
	configureButton->setEnabled(!busy);
	backupButton->setEnabled(connected && !busy);
	refreshButton->setEnabled(connected && !busy);
	const bool selected = connected && history->currentItem();
	restoreButton->setEnabled(selected && !busy);
	deleteButton->setEnabled(selected && !busy);
	exportButton->setEnabled(selected && !busy);
	importButton->setEnabled(!busy);
}

void GoogleDriveBackupDialog::SetBusy(bool value, const QString &message)
{
	busy = value;
	QWidget *widgets[] = {connectButton, configureButton, refreshButton, backupButton,
			      restoreButton, deleteButton, exportButton, importButton};
	for (QWidget *widget : widgets)
		widget->setEnabled(!value);
	if (!message.isEmpty())
		activityStatus->setText(message);
	UpdateConnectionUi();
}

void GoogleDriveBackupDialog::ConfigureClientId()
{
	bool ok = false;
	const QString current = QString::fromStdString(GoogleDriveBackupClient::Registration().clientId);
	const QString clientId = QInputDialog::getText(this, QTStr("CloudBackup.ConfigureClient"),
						      QTStr("CloudBackup.ClientId.Help"), QLineEdit::Normal,
						      current, &ok)
				 .trimmed();
	if (!ok || clientId.isEmpty())
		return;
	config_set_string(App()->GetUserConfig(), CONFIG_SECTION, "ClientId", clientId.toUtf8().constData());
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	activityStatus->setText(QTStr("CloudBackup.ClientId.Saved"));
}

void GoogleDriveBackupDialog::ConnectGoogle()
{
	if (!GoogleDriveBackupClient::RegistrationReady()) {
		QMessageBox::information(this, QTStr("CloudBackup.Title"), QTStr("CloudBackup.ClientId.Missing"));
		ConfigureClientId();
		if (!GoogleDriveBackupClient::RegistrationReady())
			return;
	}
	ClearLoopback();
	loopback = new AuthListener(this, 0);
	if (!loopback->IsListening()) {
		ClearLoopback();
		QMessageBox::critical(this, QTStr("CloudBackup.Title"), QTStr("CloudBackup.CallbackFailed"));
		return;
	}
	const string redirect = QStringLiteral("http://127.0.0.1:%1/").arg(loopback->GetPort()).toStdString();
	OAuthAuthorizationSession session;
	string nativeError;
	const auto registration = GoogleDriveBackupClient::Registration();
	if (!PlatformOAuthClient::CreateAuthorizationSessionWithScopes(
		    StreamPlatform::YouTube, registration, redirect, {GoogleDriveBackupClient::Scope}, false, session,
		    nativeError)) {
		ClearLoopback();
		QMessageBox::critical(this, QTStr("CloudBackup.Title"), QString::fromStdString(nativeError));
		return;
	}
	loopback->SetState(QString::fromStdString(session.state));
	SetBusy(true, QTStr("CloudBackup.WaitingBrowser"));
	connect(loopback, &AuthListener::fail, this, [this](const QString &reason) {
		ClearLoopback();
		SetBusy(false, reason.isEmpty() ? QTStr("CloudBackup.AuthorizationFailed") : reason);
	});
	connect(loopback, &AuthListener::ok, this, [this, registration, session](const QString &code) mutable {
		ClearLoopback();
		QPointer<GoogleDriveBackupDialog> guard(this);
		MultistreamTaskPool().start([guard, registration, session, code = code.toStdString()]() mutable {
			OAuthTokenSet tokens;
			string nativeError;
			QString error;
			QString account;
			bool success = PlatformOAuthClient::ExchangeAuthorizationCode(registration, session, code, tokens,
									       nativeError);
			if (success)
				success = GoogleDriveBackupClient::StoreTokens(tokens, error);
			if (success)
				success = GoogleDriveBackupClient::AccountName(account, error);
			if (!success && error.isEmpty())
				error = QString::fromStdString(nativeError);
			if (!guard)
				return;
			QMetaObject::invokeMethod(guard.data(), [guard, success, error]() {
				if (!guard)
					return;
				guard->SetBusy(false, success ? QTStr("CloudBackup.Connected") : error);
				guard->UpdateConnectionUi();
				if (success)
					guard->RefreshHistory();
			}, Qt::QueuedConnection);
		});
	});
	if (!QDesktopServices::openUrl(QUrl(QString::fromStdString(session.authorizationUrl)))) {
		ClearLoopback();
		SetBusy(false, QTStr("CloudBackup.BrowserFailed"));
	}
}

void GoogleDriveBackupDialog::DisconnectGoogle()
{
	if (QMessageBox::question(this, QTStr("CloudBackup.Disconnect"),
				  QTStr("CloudBackup.DisconnectConfirm")) != QMessageBox::Yes)
		return;
	QString error;
	if (!GoogleDriveBackupClient::Disconnect(error)) {
		QMessageBox::critical(this, QTStr("CloudBackup.Title"), error);
		return;
	}
	history->clear();
	UpdateConnectionUi();
	activityStatus->setText(QTStr("CloudBackup.Disconnected"));
}

void GoogleDriveBackupDialog::RefreshHistory()
{
	if (busy || !GoogleDriveBackupClient::HasConnection())
		return;
	SetBusy(true, QTStr("CloudBackup.Loading"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	MultistreamTaskPool().start([guard]() {
		vector<GoogleDriveBackupItem> items;
		QString error;
		const bool success = GoogleDriveBackupClient::List(items, error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, success, items = std::move(items), error]() mutable {
			if (!guard)
				return;
			guard->history->clear();
			if (success) {
				for (const auto &item : items) {
					auto *row = new QTreeWidgetItem(guard->history);
					row->setText(0, item.createdUtc.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm")));
					row->setText(1, item.computerName);
					row->setText(2, item.obsVersion);
					row->setText(3, HumanSize(item.size));
					row->setText(4, item.encrypted ? QTStr("CloudBackup.Encrypted")
									 : QTStr("CloudBackup.SecretsRemoved"));
					row->setData(0, Qt::UserRole, item.id);
					row->setData(0, Qt::UserRole + 1, item.name);
					row->setData(0, Qt::UserRole + 2, item.encrypted);
				}
			}
			guard->SetBusy(false, success ? QTStr("CloudBackup.HistoryLoaded").arg(items.size()) : error);
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::BackupNow()
{
	if (busy)
		return;
	SaveSettings();
	CloudBackupOptions options;
	options.generalSettings = generalCheck->isChecked();
	options.profiles = profilesCheck->isChecked();
	options.scenes = scenesCheck->isChecked();
	options.pluginSettings = pluginsCheck->isChecked();
	options.includeSecrets = secretsCheck->isChecked();
	options.passphrase = passphraseEdit->text();
	const int retention = retentionCombo->currentData().toInt();
	SetBusy(true, QTStr("CloudBackup.Creating"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	MultistreamTaskPool().start([guard, options, retention]() {
		QByteArray archive;
		CloudBackupSummary summary;
		GoogleDriveBackupItem uploaded;
		QString error;
		bool success = CloudBackupArchive::Create(options, archive, summary, error);
		if (success)
			success = GoogleDriveBackupClient::Upload(archive, summary.computerName, summary.obsVersion,
							  summary.createdUtc, summary.encrypted, uploaded, error);
		if (success) {
			QString pruneError;
			if (!GoogleDriveBackupClient::Prune(retention, pruneError))
				blog(LOG_WARNING, "Google Drive backup retention failed: %s",
				     pruneError.toUtf8().constData());
		}
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, success, error]() {
			if (!guard)
				return;
			if (success) {
				config_set_int(App()->GetUserConfig(), CONFIG_SECTION, "LastBackupUnix",
					       QDateTime::currentSecsSinceEpoch());
				config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
			}
			guard->SetBusy(false, success ? QTStr("CloudBackup.UploadComplete") : error);
			if (success)
				guard->RefreshHistory();
		}, Qt::QueuedConnection);
	});
}

GoogleDriveBackupItem GoogleDriveBackupDialog::SelectedItem() const
{
	GoogleDriveBackupItem item;
	if (QTreeWidgetItem *row = history->currentItem()) {
		item.id = row->data(0, Qt::UserRole).toString();
		item.name = row->data(0, Qt::UserRole + 1).toString();
		item.encrypted = row->data(0, Qt::UserRole + 2).toBool();
	}
	return item;
}

void GoogleDriveBackupDialog::RestoreSelected()
{
	const GoogleDriveBackupItem item = SelectedItem();
	if (item.id.isEmpty()) {
		QMessageBox::information(this, QTStr("CloudBackup.Restore"), QTStr("CloudBackup.SelectBackup"));
		return;
	}
	SetBusy(true, QTStr("CloudBackup.Downloading"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	MultistreamTaskPool().start([guard, item]() {
		QByteArray archive;
		QString error;
		const bool success = GoogleDriveBackupClient::Download(item.id, archive, error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, success, archive = std::move(archive), item, error]() mutable {
			if (!guard)
				return;
			guard->SetBusy(false, success ? QTStr("CloudBackup.DownloadComplete") : error);
			if (success)
				guard->HandleRestoreArchive(std::move(archive), item.encrypted);
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::HandleRestoreArchive(QByteArray archive, bool encryptedHint)
{
	const QByteArray magic("OBSCLOUD2");
	const bool encryptedArchive = archive.size() > magic.size() && archive.startsWith(magic) &&
				      archive.at(magic.size()) != 0;
	RestoreOptionsDialog dialog(this, encryptedArchive || encryptedHint);
	if (dialog.exec() != QDialog::Accepted)
		return;
	const CloudBackupOptions options = dialog.Options();
	if (!options.generalSettings && !options.profiles && !options.scenes && !options.pluginSettings &&
	    !options.restoreCredentials) {
		QMessageBox::information(this, QTStr("CloudBackup.Restore.Title"),
					 QTStr("CloudBackup.Restore.SelectContent"));
		return;
	}
	SetBusy(true, QTStr("CloudBackup.PreparingRestore"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	MultistreamTaskPool().start([guard, archive = std::move(archive), options]() {
		QString safety;
		QString error;
		const bool success = CloudBackupArchive::StageRestore(archive, options, safety, error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, success, safety, error]() {
			if (!guard)
				return;
			guard->SetBusy(false, success ? QTStr("CloudBackup.RestoreReady") : error);
			if (success) {
				guard->restartRequired = true;
				QMessageBox::information(guard, QTStr("CloudBackup.Restore.Title"),
						 QTStr("CloudBackup.Restore.Staged").arg(safety));
				guard->accept();
			}
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::DeleteSelected()
{
	const GoogleDriveBackupItem item = SelectedItem();
	if (item.id.isEmpty())
		return;
	if (QMessageBox::question(this, QTStr("CloudBackup.Delete"), QTStr("CloudBackup.DeleteConfirm")) !=
	    QMessageBox::Yes)
		return;
	SetBusy(true, QTStr("CloudBackup.Deleting"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	MultistreamTaskPool().start([guard, item]() {
		QString error;
		const bool success = GoogleDriveBackupClient::Remove(item.id, error);
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, success, error]() {
			if (!guard)
				return;
			guard->SetBusy(false, success ? QTStr("CloudBackup.Deleted") : error);
			if (success)
				guard->RefreshHistory();
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::ExportSelected()
{
	const GoogleDriveBackupItem item = SelectedItem();
	if (item.id.isEmpty())
		return;
	const QString path = QFileDialog::getSaveFileName(this, QTStr("CloudBackup.Export"), item.name,
							 QTStr("CloudBackup.FileFilter"));
	if (path.isEmpty())
		return;
	SetBusy(true, QTStr("CloudBackup.Downloading"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	MultistreamTaskPool().start([guard, item, path]() {
		QByteArray archive;
		QString error;
		bool success = GoogleDriveBackupClient::Download(item.id, archive, error);
		if (success) {
			QSaveFile file(path);
			success = file.open(QIODevice::WriteOnly) && file.write(archive) == archive.size() && file.commit();
			if (!success)
				error = QObject::tr("Não foi possível salvar o backup em %1.").arg(path);
		}
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, success, path, error]() {
			if (guard)
				guard->SetBusy(false, success ? QTStr("CloudBackup.Exported").arg(path) : error);
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::ImportLocal()
{
	const QString path = QFileDialog::getOpenFileName(this, QTStr("CloudBackup.Import"), {},
							 QTStr("CloudBackup.FileFilter"));
	if (path.isEmpty())
		return;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::critical(this, QTStr("CloudBackup.Import"), file.errorString());
		return;
	}
	const QByteArray archive = file.readAll();
	const bool encrypted = archive.size() > 9 && archive.at(9) != 0;
	HandleRestoreArchive(archive, encrypted);
}

void GoogleDriveBackupDialog::ClearLoopback()
{
	if (!loopback)
		return;
	loopback->deleteLater();
	loopback = nullptr;
}
