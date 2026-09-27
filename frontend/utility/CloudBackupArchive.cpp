/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "CloudBackupArchive.hpp"
#include "CloudBackupCrypto.hpp"

#include <oauth/OAuthTokenSet.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <utility/SecureTokenStore.hpp>

#include <OBSApp.hpp>

#include <util/base.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>
#include <QObject>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include <algorithm>
#include <filesystem>

using namespace std;

namespace {
constexpr char ARCHIVE_MAGIC[] = "OBSCLOUD2";
constexpr int ARCHIVE_MAGIC_SIZE = sizeof(ARCHIVE_MAGIC) - 1;
constexpr int ARCHIVE_FORMAT_VERSION = 2;
constexpr qint64 MAX_FILE_BYTES = 64LL * 1024 * 1024;
constexpr qint64 MAX_ARCHIVE_SOURCE_BYTES = 512LL * 1024 * 1024;
constexpr qint64 MAX_ARCHIVE_BYTES = 768LL * 1024 * 1024;

struct SourceRoot {
	QString archivePrefix;
	QString path;
};

QString TargetPath(const QString &archivePath);
bool ReadFile(const QString &path, QByteArray &data, QString &error);

QString ConfigRoot()
{
	return QDir::cleanPath(QString::fromStdString(
		(App()->userConfigLocation / filesystem::u8path("obs-studio")).u8string()));
}

QString GlobalConfigPath()
{
	char path[512]{};
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/global.ini") <= 0)
		return {};
	return QDir::cleanPath(QString::fromUtf8(path));
}

QString PendingRoot()
{
	char path[512]{};
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/cloud-restore/pending") <= 0)
		return {};
	return QDir::cleanPath(QString::fromUtf8(path));
}

QList<SourceRoot> SelectedRoots(const CloudBackupOptions &options)
{
	QList<SourceRoot> roots;
	if (options.profiles) {
		roots.push_back({QStringLiteral("profiles"),
				 QString::fromStdString((App()->userProfilesLocation /
							filesystem::u8path("obs-studio/basic/profiles"))
							       .u8string())});
	}
	if (options.scenes) {
		roots.push_back({QStringLiteral("scenes"),
				 QString::fromStdString((App()->userScenesLocation /
							filesystem::u8path("obs-studio/basic/scenes"))
							       .u8string())});
	}
	if (options.pluginSettings) {
		roots.push_back({QStringLiteral("plugins/config"), QDir(ConfigRoot()).filePath("plugin_config")});
		roots.push_back({QStringLiteral("plugins/manager"),
				 QString::fromStdString((App()->userPluginManagerSettingsLocation /
							filesystem::u8path("obs-studio/plugin_manager"))
							       .u8string())});
	}
	return roots;
}

bool SafeRelativePath(const QString &path)
{
	const QString clean = QDir::cleanPath(path);
	return !clean.isEmpty() && clean != QStringLiteral(".") && !QDir::isAbsolutePath(clean) &&
	       clean != QStringLiteral("..") && !clean.startsWith(QStringLiteral("../")) &&
	       !clean.contains(QChar('\0'));
}

bool IsTransientFile(const QFileInfo &info)
{
	const QString name = info.fileName().toLower();
	return name.endsWith(QStringLiteral(".tmp")) || name.endsWith(QStringLiteral(".lock")) ||
	       name.endsWith(QStringLiteral(".dmp")) || name.endsWith(QStringLiteral(".log")) ||
	       name.endsWith(QStringLiteral(".bak")) || name.endsWith(QStringLiteral(".bkp")) ||
	       name.endsWith(QStringLiteral(".journal")) || name == QStringLiteral("thumbs.db");
}

bool IsExcludedRelativePath(const QString &path)
{
	const QString normalized = QDir::fromNativeSeparators(path).toLower();
	const QStringList parts = normalized.split('/', Qt::SkipEmptyParts);
	for (const QString &part : parts) {
		if (part == QStringLiteral("cache") || part == QStringLiteral("code cache") ||
		    part == QStringLiteral("gpucache") || part == QStringLiteral("dawncache") ||
		    part == QStringLiteral("logs") || part == QStringLiteral("crashes") ||
		    part == QStringLiteral("crashdumps") || part == QStringLiteral("updates") ||
		    part == QStringLiteral("temp") || part == QStringLiteral("tmp") ||
		    part == QStringLiteral("local storage") || part == QStringLiteral("session storage") ||
		    part == QStringLiteral("indexeddb"))
			return true;
	}
	const QString name = parts.isEmpty() ? normalized : parts.constLast();
	return name == QStringLiteral("cookies") || name == QStringLiteral("login data") ||
	       name == QStringLiteral("web data") || name == QStringLiteral("network persistent state");
}

bool SensitiveJsonKey(const QString &key, bool serviceFile)
{
	QString normalized = key.toLower();
	normalized.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
	static const QSet<QString> sensitiveKeys{
		QStringLiteral("password"),      QStringLiteral("passwd"),       QStringLiteral("token"),
		QStringLiteral("accesstoken"),   QStringLiteral("refreshtoken"), QStringLiteral("idtoken"),
		QStringLiteral("authtoken"),     QStringLiteral("bearertoken"),  QStringLiteral("clientsecret"),
		QStringLiteral("secret"),        QStringLiteral("secretkey"),    QStringLiteral("apikey"),
		QStringLiteral("authorization"), QStringLiteral("credential"),   QStringLiteral("credentials"),
	};
	if (sensitiveKeys.contains(normalized))
		return true;
	return serviceFile && (normalized == QStringLiteral("key") || normalized == QStringLiteral("streamkey"));
}

QJsonValue SanitizeJson(const QJsonValue &value, bool serviceFile)
{
	if (value.isObject()) {
		QJsonObject sanitized;
		const QJsonObject object = value.toObject();
		for (auto item = object.constBegin(); item != object.constEnd(); ++item) {
			if (SensitiveJsonKey(item.key(), serviceFile))
				continue;
			sanitized.insert(item.key(), SanitizeJson(item.value(), serviceFile));
		}
		return sanitized;
	}
	if (value.isArray()) {
		QJsonArray sanitized;
		for (const QJsonValue &item : value.toArray())
			sanitized.push_back(SanitizeJson(item, serviceFile));
		return sanitized;
	}
	return value;
}

QByteArray RemoveSecrets(const QString &archivePath, const QByteArray &data)
{
	const QString lower = archivePath.toLower();
	if (lower.endsWith(QStringLiteral(".json"))) {
		QJsonParseError parseError;
		const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
		if (parseError.error == QJsonParseError::NoError) {
			const bool serviceFile = lower.endsWith(QStringLiteral("/service.json"));
			const QJsonValue clean = SanitizeJson(document.isArray() ? QJsonValue(document.array())
									      : QJsonValue(document.object()),
							 serviceFile);
			return clean.isArray() ? QJsonDocument(clean.toArray()).toJson(QJsonDocument::Compact)
					       : QJsonDocument(clean.toObject()).toJson(QJsonDocument::Compact);
		}
	}
	if (lower.endsWith(QStringLiteral(".ini"))) {
		QByteArray result;
		const QList<QByteArray> lines = data.split('\n');
		const QRegularExpression sensitive(
			QStringLiteral("^\\s*(password|passwd|token|access_?token|refresh_?token|auth_?token|"
				       "client_?secret|stream_?key|api_?key|secret(_?key)?|authorization|credentials?)\\s*="),
			QRegularExpression::CaseInsensitiveOption);
		for (const QByteArray &line : lines) {
			if (!sensitive.match(QString::fromUtf8(line)).hasMatch()) {
				result += line;
				result += '\n';
			}
		}
		if (!data.endsWith('\n') && result.endsWith('\n'))
			result.chop(1);
		return result;
	}
	return data;
}

QJsonObject ExportProtectedCredentials()
{
	QJsonArray manualKeys;
	QJsonArray oauthTokens;
	for (const MultiStreamChannel &channel : MultistreamChannelStore::Load()) {
		const StreamPlatformInfo &info = GetStreamPlatformInfo(channel.platform);
		if (info.ingestMode == StreamIngestMode::ManualStreamKey && !channel.streamKey.empty()) {
			manualKeys.push_back(QJsonObject{{QStringLiteral("channelId"), QString::fromStdString(channel.id)},
						     {QStringLiteral("streamKey"),
						      QString::fromStdString(channel.streamKey)}});
		} else if (info.ingestMode == StreamIngestMode::ResolvedByApi && !channel.accountId.empty()) {
			string error;
			auto tokens = OAuthTokenSet::Load(channel.platform, channel.accountId, error);
			if (!tokens)
				continue;
			oauthTokens.push_back(QJsonObject{
				{QStringLiteral("platform"), QString::fromUtf8(info.id.data(), info.id.size())},
				{QStringLiteral("accountId"), QString::fromStdString(channel.accountId)},
				{QStringLiteral("accessToken"), QString::fromStdString(tokens->accessToken)},
				{QStringLiteral("refreshToken"), QString::fromStdString(tokens->refreshToken)},
				{QStringLiteral("tokenType"), QString::fromStdString(tokens->tokenType)},
				{QStringLiteral("scope"), QString::fromStdString(tokens->scope)},
				{QStringLiteral("expiresAt"), static_cast<double>(tokens->expiresAt)},
				{QStringLiteral("refreshExpiresAt"), static_cast<double>(tokens->refreshExpiresAt)},
			});
		}
	}
	return {{QStringLiteral("manualKeys"), manualKeys}, {QStringLiteral("oauthTokens"), oauthTokens}};
}

bool RestoreProtectedCredentials(const QByteArray &data, QString &error)
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		error = QObject::tr("O conjunto de credenciais do backup é inválido.");
		return false;
	}
	for (const QJsonValue &value : document.object().value(QStringLiteral("manualKeys")).toArray()) {
		const QJsonObject item = value.toObject();
		const string channelId = item.value(QStringLiteral("channelId")).toString().toStdString();
		const string streamKey = item.value(QStringLiteral("streamKey")).toString().toStdString();
		if (channelId.empty() || streamKey.empty()) {
			error = QObject::tr("O conjunto de credenciais do backup é inválido.");
			return false;
		}
		string nativeError;
		if (!SecureTokenStore::Save("manual-rtmp", channelId, streamKey, nativeError)) {
			error = QString::fromStdString(nativeError);
			return false;
		}
	}
	for (const QJsonValue &value : document.object().value(QStringLiteral("oauthTokens")).toArray()) {
		const QJsonObject item = value.toObject();
		const auto platform = StreamPlatformFromId(item.value(QStringLiteral("platform")).toString().toStdString());
		const string accountId = item.value(QStringLiteral("accountId")).toString().toStdString();
		if (!platform || accountId.empty()) {
			error = QObject::tr("O conjunto de credenciais do backup é inválido.");
			return false;
		}
		OAuthTokenSet tokens;
		tokens.accessToken = item.value(QStringLiteral("accessToken")).toString().toStdString();
		tokens.refreshToken = item.value(QStringLiteral("refreshToken")).toString().toStdString();
		tokens.tokenType = item.value(QStringLiteral("tokenType")).toString().toStdString();
		tokens.scope = item.value(QStringLiteral("scope")).toString().toStdString();
		tokens.expiresAt = static_cast<int64_t>(item.value(QStringLiteral("expiresAt")).toDouble());
		tokens.refreshExpiresAt =
			static_cast<int64_t>(item.value(QStringLiteral("refreshExpiresAt")).toDouble());
		if (tokens.accessToken.empty() && tokens.refreshToken.empty()) {
			error = QObject::tr("O conjunto de credenciais do backup é inválido.");
			return false;
		}
		string nativeError;
		if (!tokens.Save(*platform, accountId, nativeError)) {
			error = QString::fromStdString(nativeError);
			return false;
		}
	}
	return true;
}

bool CaptureCredentialRollback(const QByteArray &incoming, QByteArray &rollback, QString &error)
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(incoming, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		error = QObject::tr("O conjunto de credenciais do backup é inválido.");
		return false;
	}
	QJsonArray entries;
	auto capture = [&](const string &credentialNamespace, const string &accountId) {
		if (credentialNamespace.empty() || accountId.empty())
			return false;
		string nativeError;
		auto secret = SecureTokenStore::Load(credentialNamespace, accountId, nativeError);
		if (!nativeError.empty()) {
			error = QString::fromStdString(nativeError);
			return false;
		}
		QJsonObject entry{{QStringLiteral("namespace"), QString::fromStdString(credentialNamespace)},
				  {QStringLiteral("accountId"), QString::fromStdString(accountId)},
				  {QStringLiteral("hadValue"), secret.has_value()}};
		if (secret)
			entry.insert(QStringLiteral("secret"), QString::fromStdString(*secret));
		entries.push_back(entry);
		return true;
	};
	for (const QJsonValue &value : document.object().value(QStringLiteral("manualKeys")).toArray()) {
		if (!capture("manual-rtmp", value.toObject().value(QStringLiteral("channelId")).toString().toStdString())) {
			if (error.isEmpty())
				error = QObject::tr("O conjunto de credenciais do backup é inválido.");
			return false;
		}
	}
	for (const QJsonValue &value : document.object().value(QStringLiteral("oauthTokens")).toArray()) {
		const QJsonObject item = value.toObject();
		const auto platform = StreamPlatformFromId(item.value(QStringLiteral("platform")).toString().toStdString());
		if (!platform || !capture(string(GetStreamPlatformInfo(*platform).id),
					  item.value(QStringLiteral("accountId")).toString().toStdString())) {
			if (error.isEmpty())
				error = QObject::tr("O conjunto de credenciais do backup é inválido.");
			return false;
		}
	}
	rollback = QJsonDocument(QJsonObject{{QStringLiteral("entries"), entries}}).toJson(QJsonDocument::Compact);
	return true;
}

bool RollbackProtectedCredentials(const QByteArray &data, QString &error)
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		error = QObject::tr("A cópia de segurança das credenciais é inválida.");
		return false;
	}
	for (const QJsonValue &value : document.object().value(QStringLiteral("entries")).toArray()) {
		const QJsonObject item = value.toObject();
		const string credentialNamespace = item.value(QStringLiteral("namespace")).toString().toStdString();
		const string accountId = item.value(QStringLiteral("accountId")).toString().toStdString();
		string nativeError;
		const bool restored = item.value(QStringLiteral("hadValue")).toBool()
					      ? SecureTokenStore::Save(credentialNamespace, accountId,
							       item.value(QStringLiteral("secret")).toString().toStdString(),
							       nativeError)
					      : SecureTokenStore::Remove(credentialNamespace, accountId, nativeError);
		if (!restored) {
			error = QString::fromStdString(nativeError);
			return false;
		}
	}
	error.clear();
	return true;
}

void CollectSources(const QJsonArray &sources, QHash<QString, QJsonObject> &byName)
{
	for (const QJsonValue &value : sources) {
		const QJsonObject source = value.toObject();
		const QString name = source.value(QStringLiteral("name")).toString();
		if (!name.isEmpty())
			byName.insert(name, source);
		if (source.value(QStringLiteral("sources")).isArray())
			CollectSources(source.value(QStringLiteral("sources")).toArray(), byName);
	}
}

QJsonArray PreserveDevices(const QJsonArray &incoming, const QHash<QString, QJsonObject> &local)
{
	QJsonArray merged;
	for (const QJsonValue &value : incoming) {
		QJsonObject source = value.toObject();
		const QString name = source.value(QStringLiteral("name")).toString();
		const QString id = source.value(QStringLiteral("id")).toString().toLower();
		const bool deviceSource = id.contains(QStringLiteral("capture")) || id.contains(QStringLiteral("wasapi")) ||
					  id.contains(QStringLiteral("pulse")) || id.contains(QStringLiteral("decklink")) ||
					  id.contains(QStringLiteral("coreaudio"));
		if (deviceSource && local.contains(name)) {
			QJsonObject settings = source.value(QStringLiteral("settings")).toObject();
			const QJsonObject localSettings = local.value(name).value(QStringLiteral("settings")).toObject();
			for (auto item = localSettings.constBegin(); item != localSettings.constEnd(); ++item) {
				const QString key = item.key().toLower();
				if (key.contains(QStringLiteral("device")) || key == QStringLiteral("card"))
					settings.insert(item.key(), item.value());
			}
			source.insert(QStringLiteral("settings"), settings);
		}
		if (source.value(QStringLiteral("sources")).isArray())
			source.insert(QStringLiteral("sources"),
				      PreserveDevices(source.value(QStringLiteral("sources")).toArray(), local));
		merged.push_back(source);
	}
	return merged;
}

QByteArray MergeLocalDeviceBindings(const QString &archivePath, const QByteArray &incoming)
{
	const QString target = TargetPath(archivePath);
	if (target.isEmpty() || !QFileInfo::exists(target))
		return incoming;
	QByteArray localData;
	QString ignored;
	if (!ReadFile(target, localData, ignored))
		return incoming;
	QJsonParseError incomingError;
	QJsonParseError localError;
	QJsonDocument incomingDocument = QJsonDocument::fromJson(incoming, &incomingError);
	const QJsonDocument localDocument = QJsonDocument::fromJson(localData, &localError);
	if (incomingError.error != QJsonParseError::NoError || localError.error != QJsonParseError::NoError ||
	    !incomingDocument.isObject() || !localDocument.isObject())
		return incoming;
	QHash<QString, QJsonObject> localSources;
	CollectSources(localDocument.object().value(QStringLiteral("sources")).toArray(), localSources);
	QJsonObject root = incomingDocument.object();
	root.insert(QStringLiteral("sources"),
		    PreserveDevices(root.value(QStringLiteral("sources")).toArray(), localSources));
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray PreserveIniSection(const QByteArray &incoming, const QByteArray &local, const QString &sectionName)
{
	auto splitSections = [](const QByteArray &data) {
		QList<QByteArray> blocks;
		QByteArray current;
		for (const QByteArray &line : data.split('\n')) {
			if (line.trimmed().startsWith('[') && !current.isEmpty()) {
				blocks.push_back(current);
				current.clear();
			}
			current += line;
			current += '\n';
		}
		if (!current.isEmpty())
			blocks.push_back(current);
		return blocks;
	};
	const QByteArray header = QByteArray("[") + sectionName.toUtf8() + ']';
	auto isSection = [&](const QByteArray &block) {
		const QByteArray trimmed = block.trimmed();
		return trimmed.left(header.size()).compare(header, Qt::CaseInsensitive) == 0;
	};
	QList<QByteArray> incomingBlocks = splitSections(incoming);
	QByteArray localBlock;
	for (const QByteArray &block : splitSections(local)) {
		if (isSection(block)) {
			localBlock = block;
			break;
		}
	}
	QByteArray merged;
	for (const QByteArray &block : incomingBlocks) {
		if (!isSection(block))
			merged += block;
	}
	if (!localBlock.isEmpty())
		merged += localBlock;
	return merged;
}

bool ReadFile(const QString &path, QByteArray &data, QString &error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		error = QObject::tr("Não foi possível ler %1: %2").arg(path, file.errorString());
		return false;
	}
	if (file.size() < 0 || file.size() > MAX_FILE_BYTES) {
		error = QObject::tr("O arquivo %1 excede o limite de 64 MB do backup.").arg(path);
		return false;
	}
	data = file.readAll();
	if (file.error() != QFile::NoError) {
		error = QObject::tr("Falha ao ler %1: %2").arg(path, file.errorString());
		return false;
	}
	return true;
}

bool WriteAtomic(const QString &path, const QByteArray &data, QString &error)
{
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		error = QObject::tr("Não foi possível criar a pasta de destino de %1.").arg(path);
		return false;
	}
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
		error = QObject::tr("Não foi possível gravar %1: %2").arg(path, file.errorString());
		return false;
	}
	return true;
}

bool ProtectLocalData(const QByteArray &plain, QByteArray &protectedData, QString &error)
{
#ifdef _WIN32
	DATA_BLOB input{static_cast<DWORD>(plain.size()),
			reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
	DATA_BLOB output{};
	if (!CryptProtectData(&input, L"OBS cloud restore", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
			      &output)) {
		error = QObject::tr("Não foi possível proteger as credenciais temporárias (%1).").arg(GetLastError());
		return false;
	}
	protectedData = QByteArray(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
	LocalFree(output.pbData);
	error.clear();
	return true;
#else
	Q_UNUSED(plain);
	Q_UNUSED(protectedData);
	error = QObject::tr("A proteção local de credenciais não está disponível nesta plataforma.");
	return false;
#endif
}

bool UnprotectLocalData(const QByteArray &protectedData, QByteArray &plain, QString &error)
{
#ifdef _WIN32
	DATA_BLOB input{static_cast<DWORD>(protectedData.size()),
			reinterpret_cast<BYTE *>(const_cast<char *>(protectedData.constData()))};
	DATA_BLOB output{};
	if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		error = QObject::tr("Não foi possível abrir as credenciais temporárias (%1).").arg(GetLastError());
		return false;
	}
	plain = QByteArray(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	error.clear();
	return true;
#else
	Q_UNUSED(protectedData);
	Q_UNUSED(plain);
	error = QObject::tr("A proteção local de credenciais não está disponível nesta plataforma.");
	return false;
#endif
}

bool Decode(const QByteArray &archive, const QString &passphrase, QJsonObject &root, CloudBackupSummary &summary,
	    QString &error)
{
	if (archive.size() <= ARCHIVE_MAGIC_SIZE || archive.size() > MAX_ARCHIVE_BYTES ||
	    archive.left(ARCHIVE_MAGIC_SIZE) != QByteArray(ARCHIVE_MAGIC)) {
		error = QObject::tr("Este arquivo não é um backup de nuvem compatível do OBS.");
		return false;
	}
	const bool encrypted = archive.at(ARCHIVE_MAGIC_SIZE) != 0;
	QByteArray compressed = archive.mid(ARCHIVE_MAGIC_SIZE + 1);
	if (encrypted) {
		if (passphrase.isEmpty()) {
			error = QObject::tr("Este backup contém credenciais e exige a senha definida na criação.");
			return false;
		}
		QByteArray decrypted;
		if (!CloudBackupCrypto::Decrypt(compressed, passphrase, decrypted, error))
			return false;
		compressed = std::move(decrypted);
	}
	if (compressed.size() < 4) {
		error = QObject::tr("O conteúdo do backup está incompleto.");
		return false;
	}
	const auto *length = reinterpret_cast<const unsigned char *>(compressed.constData());
	const quint32 expectedBytes = (static_cast<quint32>(length[0]) << 24) |
				      (static_cast<quint32>(length[1]) << 16) |
				      (static_cast<quint32>(length[2]) << 8) | static_cast<quint32>(length[3]);
	if (expectedBytes > static_cast<quint32>(MAX_ARCHIVE_BYTES)) {
		error = QObject::tr("O conteúdo descompactado do backup excede o limite seguro.");
		return false;
	}
	const QByteArray json = qUncompress(compressed);
	if (json.isEmpty()) {
		error = QObject::tr("O conteúdo do backup está corrompido.");
		return false;
	}
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		error = QObject::tr("O manifesto do backup é inválido.");
		return false;
	}
	root = document.object();
	if (root.value(QStringLiteral("formatVersion")).toInt() != ARCHIVE_FORMAT_VERSION ||
	    !root.value(QStringLiteral("files")).isArray()) {
		error = QObject::tr("A versão deste backup não é suportada.");
		return false;
	}
	summary.formatVersion = ARCHIVE_FORMAT_VERSION;
	summary.obsVersion = root.value(QStringLiteral("obsVersion")).toString();
	summary.computerName = root.value(QStringLiteral("computerName")).toString();
	summary.createdUtc = QDateTime::fromString(root.value(QStringLiteral("createdUtc")).toString(), Qt::ISODate);
	summary.encrypted = encrypted;
	summary.fileCount = root.value(QStringLiteral("files")).toArray().size();
	summary.unpackedBytes = static_cast<qint64>(root.value(QStringLiteral("unpackedBytes")).toDouble());
	error.clear();
	return true;
}

QString TargetPath(const QString &archivePath)
{
	QString relative;
	QString root;
	if (archivePath == QStringLiteral("general/user.ini")) {
		return QDir(ConfigRoot()).filePath(QStringLiteral("user.ini"));
	} else if (archivePath == QStringLiteral("general/global.ini")) {
		return GlobalConfigPath();
	} else if (archivePath.startsWith(QStringLiteral("profiles/"))) {
		root = QString::fromStdString((App()->userProfilesLocation /
						filesystem::u8path("obs-studio/basic/profiles"))
					       .u8string());
		relative = archivePath.mid(9);
	} else if (archivePath.startsWith(QStringLiteral("scenes/"))) {
		root = QString::fromStdString((App()->userScenesLocation /
						filesystem::u8path("obs-studio/basic/scenes"))
					       .u8string());
		relative = archivePath.mid(7);
	} else if (archivePath.startsWith(QStringLiteral("plugins/config/"))) {
		root = QDir(ConfigRoot()).filePath(QStringLiteral("plugin_config"));
		relative = archivePath.mid(15);
	} else if (archivePath.startsWith(QStringLiteral("plugins/manager/"))) {
		root = QString::fromStdString((App()->userPluginManagerSettingsLocation /
						filesystem::u8path("obs-studio/plugin_manager"))
					       .u8string());
		relative = archivePath.mid(16);
	}
	return !root.isEmpty() && SafeRelativePath(relative) ? QDir(root).filePath(relative) : QString{};
}

bool SectionSelected(const QString &path, const CloudBackupOptions &options)
{
	return (path.startsWith(QStringLiteral("general/")) && options.generalSettings) ||
	       (path.startsWith(QStringLiteral("profiles/")) && options.profiles) ||
	       (path.startsWith(QStringLiteral("scenes/")) && options.scenes) ||
	       (path.startsWith(QStringLiteral("plugins/")) && options.pluginSettings);
}
} // namespace

bool CloudBackupArchive::Create(const CloudBackupOptions &options, QByteArray &archive, CloudBackupSummary &summary,
				QString &error)
{
	if (!options.generalSettings && !options.profiles && !options.scenes && !options.pluginSettings) {
		error = QObject::tr("Selecione pelo menos uma categoria para o backup.");
		return false;
	}
	if (options.includeSecrets && options.passphrase.size() < 8) {
		error = QObject::tr("Use uma senha com pelo menos 8 caracteres para incluir credenciais.");
		return false;
	}
	if (options.includeSecrets && !EncryptionSupported()) {
		error = QObject::tr("A criptografia de credenciais não está disponível nesta plataforma.");
		return false;
	}

	QJsonArray files;
	qint64 total = 0;
	auto addData = [&](const QString &archivePath, QByteArray data) {
		if (!options.includeSecrets)
			data = RemoveSecrets(archivePath, data);
		total += data.size();
		if (total > MAX_ARCHIVE_SOURCE_BYTES) {
			error = QObject::tr("O backup excede o limite seguro de 512 MB.");
			return false;
		}
		files.push_back(QJsonObject{{QStringLiteral("path"), archivePath},
					    {QStringLiteral("size"), static_cast<double>(data.size())},
					    {QStringLiteral("sha256"),
					     QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256)
								.toHex())},
					    {QStringLiteral("data"), QString::fromLatin1(data.toBase64())}});
		return true;
	};
	auto addFile = [&](const QString &archivePath, const QString &diskPath) {
		QByteArray data;
		return ReadFile(diskPath, data, error) && addData(archivePath, std::move(data));
	};

	if (options.generalSettings) {
		const QString userIni = QDir(ConfigRoot()).filePath(QStringLiteral("user.ini"));
		if (QFileInfo::exists(userIni) && !addFile(QStringLiteral("general/user.ini"), userIni))
			return false;
		const QString globalIni = GlobalConfigPath();
		if (QFileInfo::exists(globalIni) && !addFile(QStringLiteral("general/global.ini"), globalIni))
			return false;
	}
	for (const SourceRoot &source : SelectedRoots(options)) {
		QDir root(source.path);
		if (!root.exists())
			continue;
		QDirIterator iterator(source.path, QDir::Files | QDir::NoSymLinks,
				      QDirIterator::Subdirectories);
		while (iterator.hasNext()) {
			const QString diskPath = iterator.next();
			const QFileInfo info = iterator.fileInfo();
			if (IsTransientFile(info))
				continue;
			const QString relative = root.relativeFilePath(diskPath);
			if (!SafeRelativePath(relative) || IsExcludedRelativePath(relative))
				continue;
			if (!addFile(source.archivePrefix + QLatin1Char('/') + relative, diskPath))
				return false;
		}
	}
	if (options.includeSecrets) {
		const QByteArray credentials = QJsonDocument(ExportProtectedCredentials()).toJson(QJsonDocument::Compact);
		if (!addData(QStringLiteral("protected/credentials.json"), credentials))
			return false;
	}

	const QDateTime created = QDateTime::currentDateTimeUtc();
	QJsonObject root{{QStringLiteral("formatVersion"), ARCHIVE_FORMAT_VERSION},
			 {QStringLiteral("obsVersion"), QString::fromStdString(App()->GetVersionString(false))},
			 {QStringLiteral("computerName"), QSysInfo::machineHostName()},
			 {QStringLiteral("createdUtc"), created.toString(Qt::ISODate)},
			 {QStringLiteral("unpackedBytes"), static_cast<double>(total)},
			 {QStringLiteral("files"), files}};
	QByteArray payload = qCompress(QJsonDocument(root).toJson(QJsonDocument::Compact), 9);
	const bool encrypt = options.includeSecrets;
	if (encrypt) {
#ifdef _WIN32
		QByteArray encrypted;
		if (!CloudBackupCrypto::Encrypt(payload, options.passphrase, encrypted, error))
			return false;
		payload = std::move(encrypted);
#endif
	}
	archive = QByteArray(ARCHIVE_MAGIC, ARCHIVE_MAGIC_SIZE);
	archive.append(encrypt ? char(1) : char(0));
	archive.append(payload);
	summary = {ARCHIVE_FORMAT_VERSION, QString::fromStdString(App()->GetVersionString(false)),
		   QSysInfo::machineHostName(), created, encrypt, static_cast<int>(files.size()), total};
	error.clear();
	return true;
}

bool CloudBackupArchive::Inspect(const QByteArray &archive, const QString &passphrase, CloudBackupSummary &summary,
				 QString &error)
{
	QJsonObject root;
	return Decode(archive, passphrase, root, summary, error);
}

bool CloudBackupArchive::StageRestore(const QByteArray &archive, const CloudBackupOptions &options,
				      QString &safetyBackup, QString &error)
{
	QJsonObject root;
	CloudBackupSummary incoming;
	if (!Decode(archive, options.passphrase, root, incoming, error))
		return false;

	CloudBackupOptions safetyOptions;
	safetyOptions.includeSecrets = options.restoreCredentials;
	safetyOptions.passphrase = options.passphrase;
	QByteArray safetyData;
	CloudBackupSummary safetySummary;
	if (!Create(safetyOptions, safetyData, safetySummary, error))
		return false;
	const QString backupDir = LocalBackupDirectory();
	if (!QDir().mkpath(backupDir)) {
		error = QObject::tr("Não foi possível criar a pasta local de segurança.");
		return false;
	}
	safetyBackup = QDir(backupDir).filePath(
		QStringLiteral("antes-da-restauracao-%1.obscloud")
			.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
	if (!WriteAtomic(safetyBackup, safetyData, error))
		return false;

	const QString pending = PendingRoot();
	if (pending.isEmpty()) {
		error = QObject::tr("Não foi possível localizar a pasta de restauração do OBS.");
		return false;
	}
	QDir pendingDir(pending);
	if (pendingDir.exists() && !pendingDir.removeRecursively()) {
		error = QObject::tr("Não foi possível substituir a restauração pendente anterior.");
		return false;
	}
	if (!QDir().mkpath(QDir(pending).filePath(QStringLiteral("files")))) {
		error = QObject::tr("Não foi possível preparar a restauração.");
		return false;
	}

	QJsonArray pendingFiles;
	qint64 selectedBytes = 0;
	QByteArray pendingCredentials;
	for (const QJsonValue &value : root.value(QStringLiteral("files")).toArray()) {
		const QJsonObject item = value.toObject();
		const QString path = item.value(QStringLiteral("path")).toString();
		const bool protectedCredentials = path == QStringLiteral("protected/credentials.json");
		if (!SafeRelativePath(path) || (!SectionSelected(path, options) &&
					       !(protectedCredentials && options.restoreCredentials)))
			continue;
		QByteArray data = QByteArray::fromBase64(item.value(QStringLiteral("data")).toString().toLatin1());
		const qint64 declaredSize = static_cast<qint64>(item.value(QStringLiteral("size")).toDouble(-1));
		if (declaredSize < 0 || declaredSize > MAX_FILE_BYTES || data.size() != declaredSize ||
		    selectedBytes > MAX_ARCHIVE_SOURCE_BYTES - data.size()) {
			error = QObject::tr("O tamanho do arquivo %1 no backup é inválido.").arg(path);
			return false;
		}
		selectedBytes += data.size();
		const QString hash = QString::fromLatin1(
			QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
		if (hash != item.value(QStringLiteral("sha256")).toString()) {
			error = QObject::tr("O arquivo %1 falhou na verificação de integridade.").arg(path);
			return false;
		}
		if (incoming.encrypted && !options.restoreCredentials)
			data = RemoveSecrets(path, data);
		const QString lower = path.toLower();
		if (!protectedCredentials && (lower.endsWith(QStringLiteral(".json")) ||
					     lower.endsWith(QStringLiteral(".ini")) ||
					     lower.endsWith(QStringLiteral(".txt")))) {
			QString text = QString::fromUtf8(data);
			for (const auto &mapping : options.pathMappings) {
				if (!mapping.first.isEmpty())
					text.replace(mapping.first, mapping.second, Qt::CaseInsensitive);
			}
			data = text.toUtf8();
		}
		if (path == QStringLiteral("general/global.ini")) {
			QByteArray localGlobal;
			QString ignored;
			if (ReadFile(TargetPath(path), localGlobal, ignored))
				data = PreserveIniSection(data, localGlobal, QStringLiteral("Locations"));
		}
		if (options.preserveLocalDevices && path.startsWith(QStringLiteral("scenes/")) &&
		    lower.endsWith(QStringLiteral(".json")))
			data = MergeLocalDeviceBindings(path, data);
		QJsonObject pendingItem{{QStringLiteral("path"), path}};
		if (protectedCredentials) {
			pendingCredentials = std::move(data);
		} else {
			const QString staged = QDir(pending).filePath(QStringLiteral("files/") + path);
			if (!WriteAtomic(staged, data, error))
				return false;
			const QString target = TargetPath(path);
			if (target.isEmpty()) {
				error = QObject::tr("O destino do arquivo %1 não é reconhecido.").arg(path);
				return false;
			}
			const bool hadOriginal = QFileInfo::exists(target);
			pendingItem.insert(QStringLiteral("hadOriginal"), hadOriginal);
			if (hadOriginal) {
				QByteArray original;
				if (!ReadFile(target, original, error) ||
				    !WriteAtomic(QDir(pending).filePath(QStringLiteral("rollback/") + path), original,
						 error))
					return false;
			}
		}
		pendingFiles.push_back(pendingItem);
	}
	if (pendingFiles.isEmpty()) {
		error = QObject::tr("Nenhum arquivo das categorias selecionadas existe neste backup.");
		return false;
	}
	if (!pendingCredentials.isEmpty()) {
		QByteArray rollbackCredentials;
		QByteArray protectedCredentials;
		QByteArray protectedRollback;
		if (!CaptureCredentialRollback(pendingCredentials, rollbackCredentials, error) ||
		    !ProtectLocalData(pendingCredentials, protectedCredentials, error) ||
		    !ProtectLocalData(rollbackCredentials, protectedRollback, error) ||
		    !WriteAtomic(QDir(pending).filePath(QStringLiteral("protected/credentials.bin")),
				 protectedCredentials, error) ||
		    !WriteAtomic(QDir(pending).filePath(QStringLiteral("rollback/credentials.bin")), protectedRollback,
				 error))
			return false;
		pendingCredentials.fill('\0');
		rollbackCredentials.fill('\0');
	}
	const QJsonObject marker{{QStringLiteral("formatVersion"), ARCHIVE_FORMAT_VERSION},
				 {QStringLiteral("createdUtc"), incoming.createdUtc.toString(Qt::ISODate)},
				 {QStringLiteral("files"), pendingFiles}};
	return WriteAtomic(QDir(pending).filePath(QStringLiteral("pending.json")),
			   QJsonDocument(marker).toJson(QJsonDocument::Compact), error);
}

bool CloudBackupArchive::ApplyPendingRestore(QString &error)
{
	const QString pending = PendingRoot();
	const QString markerPath = QDir(pending).filePath(QStringLiteral("pending.json"));
	if (pending.isEmpty() || !QFileInfo::exists(markerPath)) {
		error.clear();
		return true;
	}
	QByteArray markerData;
	if (!ReadFile(markerPath, markerData, error))
		return false;
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(markerData, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
	    document.object().value(QStringLiteral("formatVersion")).toInt() != ARCHIVE_FORMAT_VERSION) {
		error = QObject::tr("A restauração pendente tem um manifesto inválido.");
		return false;
	}
	const QJsonArray files = document.object().value(QStringLiteral("files")).toArray();
	/* Validate every staged file before replacing any live configuration. */
	for (const QJsonValue &value : files) {
		const QString path = value.toObject().value(QStringLiteral("path")).toString();
		if (!SafeRelativePath(path)) {
			error = QObject::tr("A restauração contém um caminho inseguro.");
			return false;
		}
		if (path == QStringLiteral("protected/credentials.json")) {
			if (!QFileInfo::exists(QDir(pending).filePath(QStringLiteral("protected/credentials.bin"))) ||
			    !QFileInfo::exists(QDir(pending).filePath(QStringLiteral("rollback/credentials.bin")))) {
				error = QObject::tr("As credenciais preparadas para restauração não foram encontradas.");
				return false;
			}
			continue;
		}
		const QString target = TargetPath(path);
		if (target.isEmpty()) {
			error = QObject::tr("O destino do arquivo %1 não é reconhecido.").arg(path);
			return false;
		}
		QByteArray data;
		if (!ReadFile(QDir(pending).filePath(QStringLiteral("files/") + path), data, error))
			return false;
	}

	QJsonArray applied;
	auto rollback = [&]() {
		for (qsizetype index = applied.size(); index > 0; --index) {
			const QJsonObject appliedItem = applied.at(index - 1).toObject();
			const QString path = appliedItem.value(QStringLiteral("path")).toString();
			const QString target = TargetPath(path);
			QString rollbackError;
			if (appliedItem.value(QStringLiteral("hadOriginal")).toBool()) {
				QByteArray original;
				if (ReadFile(QDir(pending).filePath(QStringLiteral("rollback/") + path), original,
					     rollbackError))
					WriteAtomic(target, original, rollbackError);
			} else {
				QFile::remove(target);
			}
			if (!rollbackError.isEmpty())
				blog(LOG_ERROR, "Could not roll back restored file %s: %s", path.toUtf8().constData(),
				     rollbackError.toUtf8().constData());
		}
	};
	for (const QJsonValue &value : files) {
		const QJsonObject item = value.toObject();
		const QString path = item.value(QStringLiteral("path")).toString();
		if (path == QStringLiteral("protected/credentials.json"))
			continue;
		QByteArray data;
		if (!ReadFile(QDir(pending).filePath(QStringLiteral("files/") + path), data, error) ||
		    !WriteAtomic(TargetPath(path), data, error)) {
			rollback();
			return false;
		}
		applied.push_back(item);
	}
	for (const QJsonValue &value : files) {
		const QString path = value.toObject().value(QStringLiteral("path")).toString();
		if (path != QStringLiteral("protected/credentials.json"))
			continue;
		QByteArray protectedData;
		QByteArray data;
		if (!ReadFile(QDir(pending).filePath(QStringLiteral("protected/credentials.bin")), protectedData,
			      error) ||
		    !UnprotectLocalData(protectedData, data, error) || !RestoreProtectedCredentials(data, error)) {
			QByteArray protectedRollback;
			QByteArray credentialRollback;
			QString credentialRollbackError;
			const bool credentialsRolledBack =
				ReadFile(QDir(pending).filePath(QStringLiteral("rollback/credentials.bin")),
					 protectedRollback, credentialRollbackError) &&
				UnprotectLocalData(protectedRollback, credentialRollback, credentialRollbackError) &&
				RollbackProtectedCredentials(credentialRollback, credentialRollbackError);
			if (!credentialsRolledBack) {
				blog(LOG_ERROR, "Could not roll back restored credentials: %s",
				     credentialRollbackError.toUtf8().constData());
			}
			data.fill('\0');
			credentialRollback.fill('\0');
			rollback();
			return false;
		}
		data.fill('\0');
	}
	QDir pendingDir(pending);
	if (!pendingDir.removeRecursively()) {
		blog(LOG_WARNING, "Cloud backup restore succeeded, but the pending directory could not be removed");
	}
	error.clear();
	return true;
}

bool CloudBackupArchive::EncryptionSupported()
{
	return CloudBackupCrypto::Supported();
}

bool CloudBackupArchive::HasPendingRestore()
{
	const QString pending = PendingRoot();
	return !pending.isEmpty() && QFileInfo::exists(QDir(pending).filePath(QStringLiteral("pending.json")));
}

QString CloudBackupArchive::LocalBackupDirectory()
{
	char path[512]{};
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/backups/cloud") <= 0)
		return {};
	return QDir::cleanPath(QString::fromUtf8(path));
}
