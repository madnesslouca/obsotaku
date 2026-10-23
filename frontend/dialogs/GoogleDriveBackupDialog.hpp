/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <oauth/GoogleDriveBackupClient.hpp>

#include <QDialog>
#include <functional>

class AuthListener;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

class GoogleDriveBackupDialog : public QDialog {
	Q_OBJECT

public:
	explicit GoogleDriveBackupDialog(QWidget *parent = nullptr);
	~GoogleDriveBackupDialog() override;
	bool RestartRequired() const { return restartRequired; }

private:
	void BuildUi();
	void LoadSettings();
	bool SaveSettings();
	void UpdateConnectionUi();
	void CheckConnection(bool loadHistory, const QString &message = {});
	void StartTask(std::function<void()> task);
	void SetBusy(bool busy, const QString &message = {});
	void ConnectGoogle();
	void DisconnectGoogle();
	void ConfigureClientId();
	bool CurrentContext(const std::function<bool()> &metadataCurrent,
		GoogleDriveBackupClient::SessionInfo session, bool history = false);
	void RefreshHistory();
	void BackupNow();
	void RestoreSelected();
	void DeleteSelected();
	void ExportSelected();
	void ImportLocal();
	void HandleRestoreArchive(QByteArray archive, bool encryptedHint, std::function<bool()> contextCurrent = {});
	GoogleDriveBackupItem SelectedItem() const;
	void ClearLoopback();

	AuthListener *loopback = nullptr;
	QLabel *accountStatus = nullptr;
	QLabel *activityStatus = nullptr;
	QPushButton *connectButton = nullptr;
	QPushButton *configureButton = nullptr;
	QPushButton *refreshButton = nullptr;
	QPushButton *backupButton = nullptr;
	QPushButton *restoreButton = nullptr;
	QPushButton *deleteButton = nullptr;
	QPushButton *exportButton = nullptr;
	QPushButton *importButton = nullptr;
	QCheckBox *generalCheck = nullptr;
	QCheckBox *profilesCheck = nullptr;
	QCheckBox *scenesCheck = nullptr;
	QCheckBox *pluginsCheck = nullptr;
	QCheckBox *secretsCheck = nullptr;
	QLineEdit *passphraseEdit = nullptr;
	QCheckBox *autoBackupCheck = nullptr;
	QComboBox *intervalCombo = nullptr;
	QComboBox *retentionCombo = nullptr;
	QTreeWidget *history = nullptr;
	bool busy = false;
	bool restartRequired = false;
	bool connectionChecked = false;
	bool connected = false;
	QString connectionError;
	std::function<bool()> settingsCurrent;
	std::function<bool()> connectionMetadataCurrent;
	std::function<bool()> historyMetadataCurrent;
	GoogleDriveBackupClient::SessionInfo historySession;
	GoogleDriveBackupClient::SessionInfo connectionSession;
	uint64_t connectionGeneration = 0;
	std::shared_ptr<SecureTokenStore::RequestContext> connectionRequest;
	std::shared_ptr<std::atomic_bool> httpCancelled = std::make_shared<std::atomic_bool>(false);
	std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
};
