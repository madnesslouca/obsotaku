/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

/* Versioned local backups of the scene collection file. Every save copies the
 * collection into "<scenes>/otaku_backups/<file name>/<timestamp>.json" when
 * its content changed and at least BACKUP_INTERVAL has passed. The newest
 * MAX_BACKUPS copies are kept. Restoring is done with Scene Collection ->
 * Import, and missing media can then be relinked with File -> Show Missing
 * Files. */

#include "OBSBasic.hpp"

#include <models/SceneCollection.hpp>

#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace {
constexpr int64_t BACKUP_INTERVAL_SECONDS = 10 * 60;
constexpr size_t MAX_BACKUPS = 30;

std::string ReadFile(const std::filesystem::path &path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

std::filesystem::path OtakuCollectionBackupDir(const std::filesystem::path &collectionFile)
{
	return collectionFile.parent_path() / "otaku_backups" / collectionFile.stem();
}

void OBSBasic::BackupSceneCollectionFile()
{
	const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
				    std::chrono::steady_clock::now().time_since_epoch())
				    .count();
	if (lastCollectionBackup != 0 && now - lastCollectionBackup < BACKUP_INTERVAL_SECONDS)
		return;

	std::filesystem::path source;
	try {
		source = GetCurrentSceneCollection().getFilePath();
	} catch (const std::invalid_argument &) {
		return;
	}
	std::error_code ec;
	if (!std::filesystem::exists(source, ec))
		return;

	const std::filesystem::path dir = OtakuCollectionBackupDir(source);
	std::filesystem::create_directories(dir, ec);
	if (ec) {
		blog(LOG_WARNING, "Backup: cannot create '%s': %s", dir.u8string().c_str(), ec.message().c_str());
		return;
	}

	std::vector<std::filesystem::path> existing;
	for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (entry.is_regular_file() && entry.path().extension() == ".json")
			existing.push_back(entry.path());
	}
	/* Timestamped names sort chronologically. */
	std::sort(existing.begin(), existing.end());

	const std::string content = ReadFile(source);
	if (content.empty())
		return;
	lastCollectionBackup = now;
	if (!existing.empty() && ReadFile(existing.back()) == content)
		return;

	const std::string stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss").toStdString();
	const std::filesystem::path target = dir / (stamp + ".json");
	std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		blog(LOG_WARNING, "Backup: copy failed: %s", ec.message().c_str());
		return;
	}
	existing.push_back(target);
	while (existing.size() > MAX_BACKUPS) {
		std::filesystem::remove(existing.front(), ec);
		existing.erase(existing.begin());
	}
	blog(LOG_INFO, "Backup: saved scene collection version %s", stamp.c_str());
}

void OBSBasic::OpenSceneCollectionBackups()
{
	std::filesystem::path source;
	try {
		source = GetCurrentSceneCollection().getFilePath();
	} catch (const std::invalid_argument &) {
		return;
	}
	lastCollectionBackup = 0;
	BackupSceneCollectionFile();
	const std::filesystem::path dir = OtakuCollectionBackupDir(source);
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir.u8string())));
}
