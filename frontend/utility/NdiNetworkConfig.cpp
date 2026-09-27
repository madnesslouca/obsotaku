/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "NdiNetworkConfig.hpp"

#include <OBSApp.hpp>

#include <obs.h>
#include <util/config-file.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace {
constexpr const char *CONFIG_SECTION = "NdiNetwork";
constexpr const char *CONFIG_KEY = "AdapterAddress";
constexpr const char *NDI_CONFIG_SUBDIRECTORY = "obs-studio/plugin_config/obsotaku-ndi";

QString RuntimeDirectory()
{
	return qEnvironmentVariable("NDI_RUNTIME_DIR_V6").trimmed();
}
} // namespace

std::vector<NdiNetworkAdapter> NdiNetworkConfig::AvailableAdapters()
{
	std::vector<NdiNetworkAdapter> adapters;
	QSet<QString> seenAddresses;

	for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
		if (networkInterface.flags().testFlag(QNetworkInterface::IsLoopBack))
			continue;

		for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
			if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.ip().isLoopback() ||
		    entry.ip().isNull())
				continue;

			const QString address = entry.ip().toString();
			if (address == QStringLiteral("0.0.0.0") || seenAddresses.contains(address))
				continue;
			seenAddresses.insert(address);

			NdiNetworkAdapter adapter;
			adapter.interfaceName = networkInterface.name();
			adapter.displayName = networkInterface.humanReadableName();
			if (adapter.displayName.trimmed().isEmpty())
				adapter.displayName = adapter.interfaceName;
			adapter.address = address;
			adapter.netmask = entry.netmask().toString();
			adapter.prefixLength = entry.prefixLength();
			adapter.hardwareAddress = networkInterface.hardwareAddress();
			adapter.up = networkInterface.flags().testFlag(QNetworkInterface::IsUp);
			adapter.running = networkInterface.flags().testFlag(QNetworkInterface::IsRunning);
			adapters.emplace_back(std::move(adapter));
		}
	}

	std::sort(adapters.begin(), adapters.end(), [](const NdiNetworkAdapter &left, const NdiNetworkAdapter &right) {
		if (left.running != right.running)
			return left.running > right.running;
		const int nameOrder = left.displayName.compare(right.displayName, Qt::CaseInsensitive);
		return nameOrder == 0 ? left.address < right.address : nameOrder < 0;
	});
	return adapters;
}

QString NdiNetworkConfig::SelectedAddress()
{
	const char *value = config_get_string(App()->GetAppConfig(), CONFIG_SECTION, CONFIG_KEY);
	return value ? QString::fromUtf8(value).trimmed() : QString{};
}

QString NdiNetworkConfig::ConfigDirectory()
{
	char path[1024];
	if (GetAppConfigPath(path, sizeof(path), NDI_CONFIG_SUBDIRECTORY) <= 0)
		return {};
	return QDir::fromNativeSeparators(QString::fromUtf8(path));
}

bool NdiNetworkConfig::WriteAdapterConfig(const QString &address, QString &error)
{
	const QString directory = ConfigDirectory();
	if (directory.isEmpty()) {
		error = QTStr("NdiNetwork.Error.ConfigPath");
		return false;
	}
	if (!QDir().mkpath(directory)) {
		error = QTStr("NdiNetwork.Error.CreateDirectory").arg(QDir::toNativeSeparators(directory));
		return false;
	}

	QJsonObject adapters;
	adapters.insert(QStringLiteral("allowed"), QJsonArray{address});
	QJsonObject ndi;
	ndi.insert(QStringLiteral("adapters"), adapters);
	QJsonObject root;
	root.insert(QStringLiteral("ndi"), ndi);

	QSaveFile file(QDir(directory).filePath(QStringLiteral("ndi-config.v1.json")));
	if (!file.open(QIODevice::WriteOnly)) {
		error = QTStr("NdiNetwork.Error.WriteConfig").arg(file.errorString());
		return false;
	}
	if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
		error = QTStr("NdiNetwork.Error.WriteConfig").arg(file.errorString());
		return false;
	}
	return true;
}

bool NdiNetworkConfig::SaveSelectedAddress(const QString &address, QString &error)
{
	const QString normalizedAddress = address.trimmed();
	if (!normalizedAddress.isEmpty() && !WriteAdapterConfig(normalizedAddress, error))
		return false;

	config_t *config = App()->GetAppConfig();
	const QString previous = SelectedAddress();
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY, normalizedAddress.toUtf8().constData());
	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		config_set_string(config, CONFIG_SECTION, CONFIG_KEY, previous.toUtf8().constData());
		error = QTStr("NdiNetwork.Error.SaveSettings");
		return false;
	}
	return true;
}

bool NdiNetworkConfig::PrepareEnvironment(QString *error)
{
	const QString address = SelectedAddress();
	if (address.isEmpty())
		return true;

	QString localError;
	if (!WriteAdapterConfig(address, localError)) {
		if (error)
			*error = localError;
		return false;
	}

	const QString directory = ConfigDirectory();
	if (!qputenv("NDI_CONFIG_DIR", QFile::encodeName(QDir::toNativeSeparators(directory)))) {
		if (error)
			*error = QTStr("NdiNetwork.Error.Environment");
		return false;
	}
	return true;
}

bool NdiNetworkConfig::RuntimeInstalled()
{
	const QString directory = RuntimeDirectory();
	return !directory.isEmpty() && QFileInfo::exists(QDir(directory).filePath(QStringLiteral("Processing.NDI.Lib.x64.dll")));
}

bool NdiNetworkConfig::ModuleLoaded()
{
	return obs_get_module("distroav") != nullptr;
}
