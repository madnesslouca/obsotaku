/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QString>

#include <vector>

struct NdiNetworkAdapter {
	QString interfaceName;
	QString displayName;
	QString address;
	QString netmask;
	QString hardwareAddress;
	int prefixLength = -1;
	bool up = false;
	bool running = false;
};

class NdiNetworkConfig {
public:
	static std::vector<NdiNetworkAdapter> AvailableAdapters();
	static QString SelectedAddress();
	static bool SaveSelectedAddress(const QString &address, QString &error);

	/* Must run before obs_load_all_modules2(), because the NDI SDK reads its
	 * configuration only while the DistroAV module is being initialized. */
	static bool PrepareEnvironment(QString *error = nullptr);

	static QString ConfigDirectory();
	static bool RuntimeInstalled();
	static bool ModuleLoaded();

private:
	static bool WriteAdapterConfig(const QString &address, QString &error);
};
