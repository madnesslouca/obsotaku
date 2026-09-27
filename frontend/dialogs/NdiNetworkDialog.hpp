/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/NdiNetworkConfig.hpp>

#include <QDialog>

#include <vector>

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;

class NdiNetworkDialog : public QDialog {
public:
	explicit NdiNetworkDialog(QWidget *parent = nullptr);

	bool RestartRequired() const { return restartRequired; }

private:
	void RefreshAdapters();
	void UpdateDetails();
	void SaveAndAccept();
	QFrame *CreateStatusCard(const QString &title, const QString &description, bool ready, QWidget *parent);

	std::vector<NdiNetworkAdapter> adapters;
	QString initialAddress;
	QComboBox *adapterCombo = nullptr;
	QLabel *selectionStatus = nullptr;
	QLabel *interfaceValue = nullptr;
	QLabel *addressValue = nullptr;
	QLabel *subnetValue = nullptr;
	QLabel *macValue = nullptr;
	QLabel *routeNode = nullptr;
	QPushButton *saveButton = nullptr;
	bool restartRequired = false;
};
