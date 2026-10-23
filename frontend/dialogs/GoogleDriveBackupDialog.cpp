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
#include <condition_variable>
#include <mutex>

#include "moc_GoogleDriveBackupDialog.cpp"

using namespace std;

namespace {
constexpr const char *CONFIG_SECTION = "GoogleDriveBackup";

// GUI-only scope/auth metadata snapshot; no config pointer or raw secret.
function<bool()> CloudMetadataGuard()
{
	return GoogleDriveBackupClient::CaptureConfigurationGuard();
}
bool RequestActive(const shared_ptr<SecureTokenStore::RequestContext> &request,
		   const shared_ptr<atomic_bool> &stop)
{
	return !stop->load() && !request->IsCancelled() && chrono::steady_clock::now() < request->GetDeadline();
}
struct CredentialDecision {
	mutex lock;
	condition_variable changed;
	bool decided = false;
	bool accepted = false;
	function<bool()> committedMetadataCurrent;
	QString error;
};

bool SaveCloudField(const char *key, const optional<string> &value)
{
	if (!App() || !App()->GetUserConfig()) return false;
	config_t *config = App()->GetUserConfig();
	const char *raw = config_get_string(config, CONFIG_SECTION, key);
	const optional<string> previous = config_has_user_value(config, CONFIG_SECTION, key)
		? optional<string>(raw ? raw : "") : nullopt;
	if (value) config_set_string(config, CONFIG_SECTION, key, value->c_str());
	else config_remove_value(config, CONFIG_SECTION, key);
	if (config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS) return true;
	if (previous) config_set_string(config, CONFIG_SECTION, key, previous->c_str());
	else config_remove_value(config, CONFIG_SECTION, key);
	return false;
}
struct CredentialResult {
	bool success = false;
	QString error;
	GoogleDriveBackupClient::SessionInfo session;
	function<bool()> metadataCurrent;
};
CredentialResult FinishCredentialTransaction(unique_ptr<GoogleDriveBackupClient::CredentialTransaction> lease,
	bool prepared, QString error, const shared_ptr<SecureTokenStore::RequestContext> &request,
	const shared_ptr<atomic_bool> &stop, QObject *dispatcher, function<bool(QString &)> guiCommit)
{
	CredentialResult result;
	result.error = std::move(error);
	if (prepared && RequestActive(request, stop)) {
		auto decision = make_shared<CredentialDecision>();
		QMetaObject::invokeMethod(dispatcher, [decision, request, stop, guiCommit = std::move(guiCommit)] {
			lock_guard lock(decision->lock);
			if (decision->decided) return;
			if (RequestActive(request, stop)) {
				// Local linearization is the durable save inside guiCommit. An
				// already-running save is not interrupted by cancellation.
				decision->accepted = guiCommit(decision->error);
				if (decision->accepted) decision->committedMetadataCurrent = CloudMetadataGuard();
			} else decision->error = QObject::tr("A operação foi cancelada antes de salvar a configuração.");
			decision->decided = true;
			decision->changed.notify_one();
		}, Qt::QueuedConnection);
		unique_lock lock(decision->lock);
		while (!decision->decided && RequestActive(request, stop))
			decision->changed.wait_for(lock, chrono::milliseconds(20));
		if (!decision->decided) {
			decision->decided = true;
			decision->error = QObject::tr("A operação foi cancelada antes de salvar a configuração.");
		}
		result.success = decision->accepted;
		result.metadataCurrent = std::move(decision->committedMetadataCurrent);
		result.error = decision->error;
	}
	if (lease) {
		if (result.success) result.session = lease->Commit();
		else {
			if (result.error.isEmpty()) result.error = QObject::tr("Não foi possível confirmar a operação da credencial.");
			lease->Rollback(result.error);
		}
		lease.reset(); // Same worker unlocks before the caller queues UI completion.
	}
	if (!result.success && result.error.isEmpty())
		result.error = QObject::tr("A operação foi cancelada antes de confirmar a credencial.");
	return result;
}

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
			restoreCredentials->setChecked(false);
			restoreCredentials->setEnabled(false);
			restoreCredentials->setToolTip(QTStr("CloudBackup.CredentialsPaused"));
			layout->addWidget(restoreCredentials);
			auto *restriction = new QLabel(QTStr("CloudBackup.CredentialsPaused"), this);
			restriction->setWordWrap(true);
			layout->addWidget(restriction);
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
	CheckConnection(true);
}

GoogleDriveBackupDialog::~GoogleDriveBackupDialog()
{
	alive->store(false);
	httpCancelled->store(true);
	if (connectionRequest)
		connectionRequest->Cancel();
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
		if (!connectionChecked || !connectionError.isEmpty())
			CheckConnection(false);
		else if (connected)
			DisconnectGoogle();
		else
			ConnectGoogle();
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
	secretsCheck->setChecked(false);
	secretsCheck->setEnabled(false);
	secretsCheck->setToolTip(QTStr("CloudBackup.CredentialsPaused"));
	optionsLayout->addWidget(secretsCheck);
	auto *restriction = new QLabel(QTStr("CloudBackup.CredentialsPaused"), optionsGroup);
	restriction->setWordWrap(true);
	optionsLayout->addWidget(restriction);
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
	settingsCurrent = GoogleDriveBackupClient::CaptureConfigurationGuard(GoogleDriveBackupClient::ConfigurationPart::Settings);
}

bool GoogleDriveBackupDialog::SaveSettings()
{
	if (!settingsCurrent || !settingsCurrent()) {
		activityStatus->setText(tr("As configurações mudaram. Reabra a janela antes de salvar este rascunho."));
		return false;
	}
	config_t *config = App()->GetUserConfig();
	vector<pair<string, optional<string>>> previous;
	for (const char *key : {"IncludeGeneral", "IncludeProfiles", "IncludeScenes", "IncludePlugins", "AutoEnabled", "IntervalDays", "Retention"}) {
		const char *raw = config_get_string(config, CONFIG_SECTION, key);
		previous.emplace_back(key, config_has_user_value(config, CONFIG_SECTION, key)
			? optional<string>(raw ? raw : "") : nullopt);
	}
	config_set_bool(config, CONFIG_SECTION, "IncludeGeneral", generalCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "IncludeProfiles", profilesCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "IncludeScenes", scenesCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "IncludePlugins", pluginsCheck->isChecked());
	config_set_bool(config, CONFIG_SECTION, "AutoEnabled", autoBackupCheck->isChecked());
	config_set_int(config, CONFIG_SECTION, "IntervalDays", intervalCombo->currentData().toInt());
	config_set_int(config, CONFIG_SECTION, "Retention", retentionCombo->currentData().toInt());
	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		for (const auto &[key, value] : previous) {
			if (value) config_set_string(config, CONFIG_SECTION, key.c_str(), value->c_str());
			else config_remove_value(config, CONFIG_SECTION, key.c_str());
		}
		activityStatus->setText(tr("Não foi possível salvar as configurações. O rascunho não foi aplicado."));
		return false;
	}
	settingsCurrent = GoogleDriveBackupClient::CaptureConfigurationGuard(GoogleDriveBackupClient::ConfigurationPart::Settings);
	return true;
}

void GoogleDriveBackupDialog::UpdateConnectionUi()
{
	config_t *config = App() ? App()->GetUserConfig() : nullptr;
	const char *stored = config ? config_get_string(config, CONFIG_SECTION, "AccountName") : nullptr;
	accountStatus->setText(connected ? QTStr("CloudBackup.ConnectedAs").arg(stored && *stored ? QString::fromUtf8(stored)
											 : QTStr("CloudBackup.GoogleAccount"))
					 : QTStr("CloudBackup.Disconnected"));
	if (!connectionChecked)
		accountStatus->setText(QTStr("CloudBackup.Loading"));
	else if (!connectionError.isEmpty())
		accountStatus->setText(connectionError);
	connectButton->setText(connected ? QTStr("CloudBackup.Disconnect") : QTStr("CloudBackup.Connect"));
	if (!connectionError.isEmpty())
		connectButton->setText(QTStr("CloudBackup.Refresh"));
	connectButton->setEnabled(connectionChecked && !busy);
	configureButton->setEnabled(!busy);
	const bool usable = connectionChecked && connectionError.isEmpty() && connected;
	backupButton->setEnabled(usable && !busy);
	refreshButton->setEnabled(usable && !busy);
	const bool selected = usable && history->currentItem();
	restoreButton->setEnabled(selected && !busy);
	deleteButton->setEnabled(selected && !busy);
	exportButton->setEnabled(selected && !busy);
	importButton->setEnabled(!busy);
}

void GoogleDriveBackupDialog::StartTask(function<void()> task)
{
	if (!MultistreamTaskPool().start(std::move(task))) {
		if (connectionRequest)
			connectionRequest->Cancel();
		connectionChecked = true;
		connectionError = tr("O encerramento já começou; a operação não foi iniciada.");
		SetBusy(false, connectionError);
	}
}

void GoogleDriveBackupDialog::CheckConnection(bool loadHistory, const QString &message)
{
	if (!App() || !App()->GetUserConfig()) {
		connectionChecked = true; connected = false; connectionSession = {}; connectionMetadataCurrent = {};
		connectionError = tr("A configuração não está disponível. Reabra a janela quando o perfil estiver pronto.");
		SetBusy(false, connectionError); return;
	}
	if (busy)
		return;
	if (connectionRequest)
		connectionRequest->Cancel();
	auto request = make_shared<SecureTokenStore::RequestContext>();
	connectionRequest = request;
	const auto generation = ++connectionGeneration;
	const auto metadataCurrent = CloudMetadataGuard();
	connectionChecked = false;
	connectionError.clear();
	SetBusy(true, QTStr("CloudBackup.Loading"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	StartTask([guard, dispatcher, request, generation, loadHistory, message, metadataCurrent]() {
		QString error;
		GoogleDriveBackupClient::SessionInfo session;
		const bool found = GoogleDriveBackupClient::HasConnection(error, request, &session);
		QMetaObject::invokeMethod(dispatcher, [guard, request, generation, found, error, loadHistory, message, session, metadataCurrent]() {
			if (!guard || guard->connectionGeneration != generation)
				return;
			guard->connectionRequest.reset();
			if (!metadataCurrent() || (error.isEmpty() && !GoogleDriveBackupClient::SessionCurrent(session))) {
				guard->history->clear();
				guard->SetBusy(false);
				guard->CheckConnection(loadHistory);
				return;
			}
			if (guard->connectionSession.epoch != session.epoch) guard->history->clear();
			guard->connectionChecked = true;
			guard->connected = found;
			guard->connectionSession = session;
			guard->connectionMetadataCurrent = error.isEmpty() ? metadataCurrent : function<bool()>{};
			guard->connectionError = error;
			guard->SetBusy(false, !error.isEmpty() ? error : (!message.isEmpty() ? message : QTStr("CloudBackup.Ready")));
			if (found && error.isEmpty() && loadHistory)
				guard->RefreshHistory();
		}, Qt::QueuedConnection);
	});
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
	const auto metadataCurrent = CloudMetadataGuard();
	if (!metadataCurrent()) { activityStatus->setText(tr("A configuração não está disponível.")); return; }
	bool ok = false;
	const QString current = QString::fromStdString(GoogleDriveBackupClient::Registration().clientId);
	const QString clientId = QInputDialog::getText(this, QTStr("CloudBackup.ConfigureClient"),
						      QTStr("CloudBackup.ClientId.Help"), QLineEdit::Normal,
						      current, &ok)
				 .trimmed();
	if (!ok || clientId.isEmpty())
		return;
	if (!metadataCurrent()) {
		activityStatus->setText(tr("A configuração mudou enquanto a janela estava aberta. Abra a edição novamente."));
		return;
	}
	config_t *config = App()->GetUserConfig();
	const char *raw = config_get_string(config, CONFIG_SECTION, "ClientId");
	const optional<string> previous = config_has_user_value(config, CONFIG_SECTION, "ClientId")
		? optional<string>(raw ? raw : "") : nullopt;
	config_set_string(config, CONFIG_SECTION, "ClientId", clientId.toUtf8().constData());
	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		if (previous) config_set_string(config, CONFIG_SECTION, "ClientId", previous->c_str());
		else config_remove_value(config, CONFIG_SECTION, "ClientId");
		activityStatus->setText(tr("Não foi possível salvar o Client ID. A configuração anterior foi mantida."));
		return;
	}
	activityStatus->setText(QTStr("CloudBackup.ClientId.Saved"));
	CheckConnection(false, QTStr("CloudBackup.ClientId.Saved"));
}

void GoogleDriveBackupDialog::ConnectGoogle()
{
	if (!GoogleDriveBackupClient::RegistrationReady()) {
		QMessageBox::information(this, QTStr("CloudBackup.Title"), QTStr("CloudBackup.ClientId.Missing"));
		ConfigureClientId();
		if (!GoogleDriveBackupClient::RegistrationReady())
			return;
	}
	const auto metadataCurrent = connectionMetadataCurrent;
	const auto previousSession = connectionSession;
	if (!metadataCurrent || !metadataCurrent() || !GoogleDriveBackupClient::SessionCurrent(previousSession)) {
		CheckConnection(false, tr("A conexão mudou. Confira a conta antes de autorizar.")); return;
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
	const OAuthHttpClient::Cancellation cancel{httpCancelled, MultistreamTaskPool().StopToken()};
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
	connect(loopback, &AuthListener::ok, this, [this, registration, session, cancel, metadataCurrent, previousSession](const QString &code) mutable {
		ClearLoopback();
		if (!metadataCurrent || !metadataCurrent() || !GoogleDriveBackupClient::SessionCurrent(previousSession)) {
			SetBusy(false); CheckConnection(false, tr("A conexão mudou durante a autorização.")); return;
		}
		auto request = make_shared<SecureTokenStore::RequestContext>();
		connectionRequest = request;
		const auto generation = ++connectionGeneration;
		QPointer<GoogleDriveBackupDialog> guard(this);
		QObject *dispatcher = MultistreamGuiDispatcher();
		const auto stop = MultistreamTaskPool().StopToken();
		StartTask([guard, dispatcher, request, generation, cancel, stop, registration, session, metadataCurrent, previousSession, code = code.toStdString()]() mutable {
			OAuthTokenSet tokens;
			string nativeError;
			QString error, account;
			bool success = false;
			if (RequestActive(request, stop) && !cancel.IsCancelled()) {
				// Do not start a noncancellable exchange after the intent expired.
				// An exchange already in flight keeps its existing completion rule.
				success = PlatformOAuthClient::ExchangeAuthorizationCode(registration, session, code, tokens, nativeError);
				if (!success) error = QString::fromStdString(nativeError);
			} else error = QObject::tr("A autorização foi cancelada antes de iniciar a troca do código.");
			if (success) success = GoogleDriveBackupClient::AuthorizedAccountName(tokens, account, error, cancel);
			unique_ptr<GoogleDriveBackupClient::CredentialTransaction> lease;
			if (success && RequestActive(request, stop) && !cancel.IsCancelled())
				lease = GoogleDriveBackupClient::BeginCredentialTransaction(previousSession, error, request);
			const bool stored = lease && RequestActive(request, stop) && !cancel.IsCancelled() && lease->Store(tokens, error);
			auto result = FinishCredentialTransaction(std::move(lease), stored, error, request, stop, dispatcher,
				[guard, request, generation, stop, metadataCurrent, previousSession, account](QString &error) {
					if (!guard || guard->connectionGeneration != generation || !RequestActive(request, stop) ||
					    !metadataCurrent() || !GoogleDriveBackupClient::SessionCurrent(previousSession)) {
						error = QObject::tr("A configuração mudou antes de salvar a autorização."); return false;
					}
					if (!SaveCloudField("AccountName", account.toStdString())) {
						error = QObject::tr("Não foi possível salvar a conta. A credencial anterior será mantida."); return false;
					}
					return true;
				});
			QMetaObject::invokeMethod(dispatcher, [guard, generation, result = std::move(result)]() mutable {
				if (!guard || guard->connectionGeneration != generation) return;
				guard->connectionRequest.reset();
				if (!result.success || !GoogleDriveBackupClient::SessionCurrent(result.session) ||
				    !result.metadataCurrent || !result.metadataCurrent()) {
					guard->SetBusy(false, result.error); guard->CheckConnection(false, result.error); return;
				}
				guard->connected = true; guard->connectionChecked = true;
				guard->connectionSession = result.session; guard->connectionMetadataCurrent = result.metadataCurrent; guard->connectionError.clear();
				guard->SetBusy(false, QTStr("CloudBackup.Connected")); guard->RefreshHistory();
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
	const auto metadataCurrent = connectionMetadataCurrent;
	const auto session = connectionSession;
	if (QMessageBox::question(this, QTStr("CloudBackup.Disconnect"),
				  QTStr("CloudBackup.DisconnectConfirm")) != QMessageBox::Yes) return;
	if (!metadataCurrent || !metadataCurrent() || !GoogleDriveBackupClient::SessionCurrent(session)) {
		CheckConnection(false, tr("A conexão mudou. Confira a conta antes de desconectar."));
		return;
	}
	auto request = make_shared<SecureTokenStore::RequestContext>();
	connectionRequest = request;
	const auto generation = ++connectionGeneration;
	SetBusy(true, QTStr("CloudBackup.Loading"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	const auto stop = MultistreamTaskPool().StopToken();
	StartTask([guard, dispatcher, request, generation, metadataCurrent, session, stop]() {
		QString error;
		unique_ptr<GoogleDriveBackupClient::CredentialTransaction> lease;
		if (RequestActive(request, stop)) lease = GoogleDriveBackupClient::BeginCredentialTransaction(session, error, request);
		const bool removed = lease && RequestActive(request, stop) && lease->Remove(error);
		auto result = FinishCredentialTransaction(std::move(lease), removed, error, request, stop, dispatcher,
			[guard, request, generation, metadataCurrent, session, stop](QString &error) {
				if (!guard || guard->connectionGeneration != generation || !RequestActive(request, stop) ||
				    !metadataCurrent() || !GoogleDriveBackupClient::SessionCurrent(session)) {
					error = QObject::tr("A conexão mudou antes de confirmar a desconexão."); return false;
				}
				if (!SaveCloudField("AccountName", nullopt)) {
					error = QObject::tr("Não foi possível salvar a desconexão. A credencial será restaurada."); return false;
				}
				return true;
			});
		QMetaObject::invokeMethod(dispatcher, [guard, generation, result = std::move(result)]() mutable {
			if (!guard || guard->connectionGeneration != generation) return;
			guard->connectionRequest.reset();
			if (result.success && (!GoogleDriveBackupClient::SessionCurrent(result.session) ||
				!result.metadataCurrent || !result.metadataCurrent())) {
				guard->SetBusy(false); guard->CheckConnection(false); return;
			}
			if (result.success) {
				guard->connected = false; guard->connectionChecked = true;
				guard->connectionSession = result.session;
				guard->connectionMetadataCurrent = result.metadataCurrent;
				guard->connectionError.clear(); guard->history->clear();
				guard->SetBusy(false, QTStr("CloudBackup.Disconnected"));
			} else { guard->SetBusy(false, result.error); guard->CheckConnection(false, result.error); }
		}, Qt::QueuedConnection);
	});
}

bool GoogleDriveBackupDialog::CurrentContext(const function<bool()> &metadataCurrent,
	GoogleDriveBackupClient::SessionInfo session, bool loadHistory)
{
	if (metadataCurrent && metadataCurrent() && connectionMetadataCurrent && connectionMetadataCurrent() &&
	    GoogleDriveBackupClient::SessionCurrent(session)) return true;
	history->clear();
	SetBusy(false, tr("A configuração ou a conta mudou. O resultado antigo não foi aplicado à interface atual."));
	CheckConnection(loadHistory);
	return false;
}

void GoogleDriveBackupDialog::RefreshHistory()
{
	if (busy || !connectionChecked || !connectionError.isEmpty() || !connected)
		return;
	const auto metadataCurrent = connectionMetadataCurrent;
	const auto session = connectionSession;
	if (!CurrentContext(metadataCurrent, session)) return;
	SetBusy(true, QTStr("CloudBackup.Loading"));
	const auto registration = GoogleDriveBackupClient::Registration();
	const OAuthHttpClient::Cancellation cancel{httpCancelled, MultistreamTaskPool().StopToken()};
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	StartTask([cancel, guard, dispatcher, registration, metadataCurrent, session]() {
		vector<GoogleDriveBackupItem> items;
		QString error;
		const bool success = GoogleDriveBackupClient::List(items, error, registration, cancel, session);
		QMetaObject::invokeMethod(dispatcher, [guard, success, items = std::move(items), error, metadataCurrent, session]() mutable {
			if (!guard) return;
			if (!guard->CurrentContext(metadataCurrent, session, true)) return;
			guard->history->clear();
			guard->historySession = session;
			guard->historyMetadataCurrent = metadataCurrent;
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
	const auto session = connectionSession;
	if (!CurrentContext(connectionMetadataCurrent, session)) return;
	CloudBackupRoots roots;
	QString rootsError;
	if (!CloudBackupArchive::CaptureRoots(roots, rootsError)) { activityStatus->setText(rootsError); return; }
	if (!SaveSettings()) return;
	const auto metadataCurrent = GoogleDriveBackupClient::CaptureConfigurationGuard(GoogleDriveBackupClient::ConfigurationPart::All);
	CloudBackupOptions options;
	options.generalSettings = generalCheck->isChecked();
	options.profiles = profilesCheck->isChecked();
	options.scenes = scenesCheck->isChecked();
	options.pluginSettings = pluginsCheck->isChecked();
	options.includeSecrets = secretsCheck->isChecked();
	options.passphrase = passphraseEdit->text();
	const int retention = retentionCombo->currentData().toInt();
	SetBusy(true, QTStr("CloudBackup.Creating"));
	const auto registration = GoogleDriveBackupClient::Registration();
	const OAuthHttpClient::Cancellation cancel{httpCancelled, MultistreamTaskPool().StopToken()};
	auto request = make_shared<SecureTokenStore::RequestContext>();
	connectionRequest = request;
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	StartTask([cancel, guard, dispatcher, registration, options, retention, request, metadataCurrent, session, roots]() {
		QByteArray archive;
		CloudBackupSummary summary;
		GoogleDriveBackupItem uploaded;
		QString error;
		bool success = !cancel.IsCancelled() && GoogleDriveBackupClient::ValidateSession(session, error, request);
		if (success) success = CloudBackupArchive::Create(roots, options, archive, summary, error, [cancel] { return cancel.IsCancelled(); });
		if (success)
			success = GoogleDriveBackupClient::Upload(archive, summary.computerName, summary.obsVersion,
							  summary.createdUtc, summary.encrypted, uploaded, error, registration, cancel, session);
		if (success) {
			QString pruneError;
			if (!GoogleDriveBackupClient::Prune(retention, pruneError, registration, cancel, session))
				blog(LOG_WARNING, "Google Drive backup retention failed.");
		}
		QMetaObject::invokeMethod(dispatcher, [guard, success, error, metadataCurrent, session, roots]() {
			if (!guard) return;
			guard->connectionRequest.reset();
			if (!guard->CurrentContext(metadataCurrent, session)) return;
			QString rootsError;
			if (!CloudBackupArchive::RootsCurrent(roots, rootsError)) { guard->SetBusy(false, rootsError); return; }
			if (success) {
				if (!SaveCloudField("LastBackupUnix", to_string(QDateTime::currentSecsSinceEpoch()))) {
					guard->SetBusy(false, tr("O upload terminou, mas não foi possível salvar a data do backup.")); return;
				}
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
	if (busy) return; // Also covers the history itemDoubleClicked gesture.
	CloudBackupRoots roots;
	QString rootsError;
	if (!CloudBackupArchive::CaptureRoots(roots, rootsError)) { activityStatus->setText(rootsError); return; }
	const auto metadataCurrent = historyMetadataCurrent;
	const auto session = historySession;
	const auto registration = GoogleDriveBackupClient::Registration();
	const GoogleDriveBackupItem item = SelectedItem();
	if (item.id.isEmpty()) {
		QMessageBox::information(this, QTStr("CloudBackup.Restore"), QTStr("CloudBackup.SelectBackup"));
		return;
	}
	if (!CurrentContext(metadataCurrent, session)) return;
	SetBusy(true, QTStr("CloudBackup.Downloading"));
	const OAuthHttpClient::Cancellation cancel{httpCancelled, MultistreamTaskPool().StopToken()};
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	StartTask([cancel, guard, dispatcher, registration, item, metadataCurrent, session, roots]() {
		QByteArray archive;
		QString error;
		const bool success = GoogleDriveBackupClient::Download(item.id, archive, error, registration, cancel, session);
		QMetaObject::invokeMethod(dispatcher, [guard, success, archive = std::move(archive), item, error, metadataCurrent, session, roots]() mutable {
			if (!guard || !guard->CurrentContext(metadataCurrent, session)) return;
			QString rootsError;
			if (!CloudBackupArchive::RootsCurrent(roots, rootsError)) { guard->SetBusy(false, rootsError); return; }
			guard->SetBusy(false, success ? QTStr("CloudBackup.DownloadComplete") : error);
			if (success)
				guard->HandleRestoreArchive(std::move(archive), item.encrypted, [metadataCurrent, session, roots] { QString ignored; return metadataCurrent && metadataCurrent() && GoogleDriveBackupClient::SessionCurrent(session) && CloudBackupArchive::RootsCurrent(roots, ignored); });
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::HandleRestoreArchive(QByteArray archive, bool encryptedHint, function<bool()> contextCurrent)
{
	if (!contextCurrent) contextCurrent = CloudMetadataGuard();
	CloudBackupRoots roots;
	QString rootsError;
	if (!CloudBackupArchive::CaptureRoots(roots, rootsError)) { activityStatus->setText(rootsError); return; }
	const auto sourceCurrent = contextCurrent;
	contextCurrent = [sourceCurrent, roots] {
		QString ignored;
		return sourceCurrent && sourceCurrent() && CloudBackupArchive::RootsCurrent(roots, ignored);
	};
	if (!contextCurrent()) { activityStatus->setText(tr("A configuração mudou antes de restaurar.")); return; }
	const QByteArray magic("OBSCLOUD2");
	const bool encryptedArchive = archive.size() > magic.size() && archive.startsWith(magic) &&
				      archive.at(magic.size()) != 0;
	RestoreOptionsDialog dialog(this, encryptedArchive || encryptedHint);
	if (dialog.exec() != QDialog::Accepted)
		return;
	if (!contextCurrent()) { activityStatus->setText(tr("A configuração mudou durante a escolha da restauração.")); return; }
	const CloudBackupOptions options = dialog.Options();
	if (!options.generalSettings && !options.profiles && !options.scenes && !options.pluginSettings &&
	    !options.restoreCredentials) {
		QMessageBox::information(this, QTStr("CloudBackup.Restore.Title"),
					 QTStr("CloudBackup.Restore.SelectContent"));
		return;
	}
	SetBusy(true, QTStr("CloudBackup.PreparingRestore"));
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	const OAuthHttpClient::Cancellation cancel{httpCancelled, MultistreamTaskPool().StopToken()};
	StartTask([guard, dispatcher, archive = std::move(archive), options, roots, contextCurrent, cancel]() {
		QString safety;
		QString error;
		const bool success = CloudBackupArchive::StageRestore(roots, archive, options, safety, error, [cancel] { return cancel.IsCancelled(); });
		QMetaObject::invokeMethod(dispatcher, [guard, success, safety, error, contextCurrent]() {
			if (!guard)
				return;
			if (!contextCurrent()) {
				guard->SetBusy(false, tr("O contexto mudou. Uma intenção já publicada permanece vinculada somente às raízes originais.")); return;
			}
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
	const auto metadataCurrent = historyMetadataCurrent;
	const auto session = historySession;
	const auto registration = GoogleDriveBackupClient::Registration();
	const GoogleDriveBackupItem item = SelectedItem();
	if (item.id.isEmpty())
		return;
	if (QMessageBox::question(this, QTStr("CloudBackup.Delete"), QTStr("CloudBackup.DeleteConfirm")) !=
	    QMessageBox::Yes)
		return;
	if (!CurrentContext(metadataCurrent, session)) return;
	SetBusy(true, QTStr("CloudBackup.Deleting"));
	const OAuthHttpClient::Cancellation cancel{httpCancelled, MultistreamTaskPool().StopToken()};
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	StartTask([cancel, guard, dispatcher, registration, item, metadataCurrent, session]() {
		QString error;
		const bool success = GoogleDriveBackupClient::Remove(item.id, error, registration, cancel, session);
		QMetaObject::invokeMethod(dispatcher, [guard, success, error, metadataCurrent, session]() {
			if (!guard || !guard->CurrentContext(metadataCurrent, session)) return;
			guard->SetBusy(false, success ? QTStr("CloudBackup.Deleted") : error);
			if (success)
				guard->RefreshHistory();
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::ExportSelected()
{
	const auto metadataCurrent = historyMetadataCurrent;
	const auto session = historySession;
	const auto registration = GoogleDriveBackupClient::Registration();
	const GoogleDriveBackupItem item = SelectedItem();
	if (item.id.isEmpty())
		return;
	const QString path = QFileDialog::getSaveFileName(this, QTStr("CloudBackup.Export"), item.name,
							 QTStr("CloudBackup.FileFilter"));
	if (path.isEmpty())
		return;
	if (!CurrentContext(metadataCurrent, session)) return;
	SetBusy(true, QTStr("CloudBackup.Downloading"));
	const OAuthHttpClient::Cancellation cancel{httpCancelled, MultistreamTaskPool().StopToken()};
	QPointer<GoogleDriveBackupDialog> guard(this);
	QObject *dispatcher = MultistreamGuiDispatcher();
	StartTask([cancel, guard, dispatcher, registration, item, path, metadataCurrent, session]() {
		QByteArray archive;
		QString error;
		bool success = GoogleDriveBackupClient::Download(item.id, archive, error, registration, cancel, session);
		if (success) {
			QSaveFile file(path);
			success = file.open(QIODevice::WriteOnly) && file.write(archive) == archive.size() && file.commit();
			if (!success)
				error = QObject::tr("Não foi possível salvar o backup em %1.").arg(path);
		}
		QMetaObject::invokeMethod(dispatcher, [guard, success, path, error, metadataCurrent, session]() {
			if (guard && guard->CurrentContext(metadataCurrent, session))
				guard->SetBusy(false, success ? QTStr("CloudBackup.Exported").arg(path) : error);
		}, Qt::QueuedConnection);
	});
}

void GoogleDriveBackupDialog::ImportLocal()
{
	if (busy) return;
	CloudBackupRoots roots;
	QString rootsError;
	if (!CloudBackupArchive::CaptureRoots(roots, rootsError)) { activityStatus->setText(rootsError); return; }
	const auto metadataCurrent = CloudMetadataGuard();
	const auto contextCurrent = [metadataCurrent, roots] {
		QString ignored;
		return metadataCurrent() && CloudBackupArchive::RootsCurrent(roots, ignored);
	};
	const QString path = QFileDialog::getOpenFileName(this, QTStr("CloudBackup.Import"), {},
							 QTStr("CloudBackup.FileFilter"));
	if (path.isEmpty()) return;
	if (!contextCurrent()) { activityStatus->setText(tr("A configuração mudou durante a escolha do arquivo.")); return; }
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::critical(this, QTStr("CloudBackup.Import"), file.errorString());
		return;
	}
	const QByteArray archive = file.readAll();
	const bool encrypted = archive.size() > 9 && archive.at(9) != 0;
	HandleRestoreArchive(archive, encrypted, contextCurrent);
}

void GoogleDriveBackupDialog::ClearLoopback()
{
	if (!loopback)
		return;
	loopback->deleteLater();
	loopback = nullptr;
}
