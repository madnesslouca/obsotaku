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
	void SaveSettings();
	void UpdateConnectionUi();
	void SetBusy(bool busy, const QString &message = {});
	void ConnectGoogle();
	void DisconnectGoogle();
	void ConfigureClientId();
	void RefreshHistory();
	void BackupNow();
	void RestoreSelected();
	void DeleteSelected();
	void ExportSelected();
	void ImportLocal();
	void HandleRestoreArchive(QByteArray archive, bool encryptedHint);
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
};
