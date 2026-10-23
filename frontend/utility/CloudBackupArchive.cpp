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
#include <QLockFile>
#include <QStandardPaths>
#include <QUuid>
#include <QSet>
#include <QSysInfo>
#include <QObject>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/stat.h>
#endif

#include <algorithm>
#include <filesystem>

using namespace std;

namespace {
constexpr char ARCHIVE_MAGIC[] = "OBSCLOUD2";
constexpr int ARCHIVE_MAGIC_SIZE = sizeof(ARCHIVE_MAGIC) - 1;
constexpr int ARCHIVE_FORMAT_VERSION = 2;
constexpr int LOCAL_PENDING_VERSION = 3;
constexpr qint64 MAX_FILE_BYTES = 64LL * 1024 * 1024;
constexpr qint64 MAX_ARCHIVE_SOURCE_BYTES = 512LL * 1024 * 1024;
constexpr qint64 MAX_ARCHIVE_BYTES = 768LL * 1024 * 1024;

struct SourceRoot {
	QString archivePrefix;
	QString path;
};

QString TargetPath(const CloudBackupRoots &roots, const QString &archivePath);
bool ReadFile(const QString &path, QByteArray &data, QString &error);

/* Resolve relative portable paths lexically at capture time. Filesystem alias
 * validation remains in the worker/startup checks below. */
QString AbsoluteAppPath(const QString &path)
{
	if (path.isEmpty() || path.contains(QChar('\0')))
		return {};
	return QDir::cleanPath(QDir::current().absoluteFilePath(path));
}

QString AppGlobalPath(const char *relative)
{
	char path[512]{};
	if (GetAppConfigPath(path, sizeof(path), relative) <= 0)
		return {};
	return AbsoluteAppPath(QString::fromUtf8(path));
}

QString AppPath(const filesystem::path &base, const char *relative)
{
	if (base.empty())
		return {};
	return AbsoluteAppPath(QString::fromStdString((base / filesystem::u8path(relative)).u8string()));
}

/* Fail closed on symbolic links/reparse points, including ancestors and leaf
 * aliases. For absent roots, resolve the nearest existing ancestor then append
 * the absent suffix. Lexical path AND resolution are bound, so case/alias
 * changes may require restaging rather than silently selecting a new target. */
bool PathIdentity(const QString &path, QString &resolved, QString &error)
{
	if (path.isEmpty() || !QDir::isAbsolutePath(path) || path.contains(QChar('\0')) ||
	    QDir::cleanPath(path) != path) {
		error = QObject::tr("A raiz local de restauração não é absoluta ou está inválida.");
		return false;
	}
	QString cursor = path;
	QStringList suffix;
	while (true) {
		const QFileInfo info(cursor);
		bool alias = info.isSymLink();
#ifdef _WIN32
		const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(cursor.utf16()));
		if (attributes != INVALID_FILE_ATTRIBUTES)
			alias = alias || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#endif
		if (alias) {
			error = QObject::tr("Uma raiz ou destino usa um alias, link simbólico ou reparse point. Preserve a restauração e escolha raízes diretas.");
			return false;
		}
		if (info.exists()) {
			const QString canonical = info.canonicalFilePath();
			if (canonical.isEmpty() || (!suffix.isEmpty() && !info.isDir())) {
				error = QObject::tr("Não foi possível identificar a raiz local de restauração.");
				return false;
			}
			resolved = canonical;
			for (const QString &part : suffix)
				resolved = QDir(resolved).filePath(part);
			break;
		}
		const QString parent = info.absolutePath();
		if (parent == cursor || parent.isEmpty() || info.fileName().isEmpty()) {
			error = QObject::tr("Não foi possível resolver a raiz local de restauração.");
			return false;
		}
		suffix.prepend(info.fileName());
		cursor = parent;
	}
	// Existing ancestors must also be direct, even if canonicalFilePath hides an alias.
	cursor = path;
	while (true) {
		const QFileInfo info(cursor);
		bool alias = info.isSymLink();
#ifdef _WIN32
		const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(cursor.utf16()));
		if (attributes != INVALID_FILE_ATTRIBUTES)
			alias = alias || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#endif
		if (alias) { error = QObject::tr("A restauração não aceita aliases nas raízes locais."); return false; }
		const QString parent = info.absolutePath();
		if (parent == cursor) break;
		cursor = parent;
	}
	resolved = QDir::cleanPath(resolved);
	return true;
}

QList<QPair<QString, QString>> RootPaths(const CloudBackupRoots &roots)
{
	return {{"config", roots.configRoot}, {"global", roots.globalConfigPath},
		{"profiles", roots.profilesRoot}, {"scenes", roots.scenesRoot},
		{"pluginConfig", roots.pluginConfigRoot}, {"pluginManager", roots.pluginManagerRoot},
		{"pending", roots.pendingRoot}, {"safety", roots.safetyRoot}, {"coordination", roots.coordinationRoot}};
}

bool DirectoryObject(const QString &path, QString &identity, QString &error)
{
#ifdef _WIN32
	HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { error = QObject::tr("Não foi possível identificar o diretório local (%1).").arg(GetLastError()); return false; }
	BY_HANDLE_FILE_INFORMATION info{};
	const bool read = GetFileInformationByHandle(handle, &info);
	CloseHandle(handle);
	if (!read || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	    !info.dwVolumeSerialNumber || (!info.nFileIndexHigh && !info.nFileIndexLow)) {
		error = QObject::tr("O volume ou identificador do diretório local é ambíguo."); return false;
	}
	identity = QStringLiteral("win:%1:%2:%3").arg(static_cast<qulonglong>(info.dwVolumeSerialNumber)).arg(static_cast<qulonglong>(info.nFileIndexHigh)).arg(static_cast<qulonglong>(info.nFileIndexLow));
#else
	struct stat info{};
	const QByteArray native = QFile::encodeName(path);
	if (::lstat(native.constData(), &info) != 0 || !S_ISDIR(info.st_mode) || !info.st_ino) {
		error = QObject::tr("Não foi possível identificar o diretório local pelo dispositivo e inode."); return false;
	}
	identity = QStringLiteral("posix:%1:%2").arg(static_cast<qulonglong>(info.st_dev)).arg(static_cast<qulonglong>(info.st_ino));
#endif
	return true;
}

bool RootDescriptor(const QString &path, bool fileRoot, QJsonObject &descriptor, QString &error)
{
	QString resolved;
	if (!PathIdentity(path, resolved, error)) return false;
	if (fileRoot && QFileInfo::exists(path) && !QFileInfo(path).isFile()) {
		error = QObject::tr("A configuração global não é um arquivo regular."); return false;
	}
	QString cursor = fileRoot ? QFileInfo(path).absolutePath() : path;
	QStringList missing;
	while (!QFileInfo::exists(cursor)) {
		const QFileInfo info(cursor);
		const QString parent = info.absolutePath();
		if (parent == cursor || info.fileName().isEmpty()) { error = QObject::tr("Não há um ancestral local identificável."); return false; }
		missing.prepend(info.fileName()); cursor = parent;
	}
	QString object;
	if (!DirectoryObject(cursor, object, error)) return false;
	descriptor = QJsonObject{{"path", path}, {"resolved", resolved}, {"anchor", cursor},
		{"object", object}, {"missing", QJsonArray::fromStringList(missing)}, {"fileRoot", fileRoot}};
	return true;
}

bool RootBinding(const CloudBackupRoots &roots, QString &identity, QString &error, QJsonObject *descriptors = nullptr, bool runtime = false)
{
	QJsonObject object{{"bindingVersion", 2}, {"obsVersion", roots.obsVersion}};
	if (roots.obsVersion.isEmpty()) { error = QObject::tr("A versão local não foi identificada."); return false; }
	QJsonObject paths;
	for (const auto &path : RootPaths(roots)) {
		if (!runtime && path.first == "coordination") continue; // Ephemeral cache is not a persistent destination.
		QJsonObject descriptor;
		if (!RootDescriptor(path.second, path.first == "global", descriptor, error)) return false;
		paths.insert(path.first, descriptor);
	}
	object.insert("roots", paths);
	identity = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
	if (descriptors) *descriptors = object;
	return true;
}

/* An operation pins native identities of every already-existing anchor. A
 * missing suffix becoming present is accepted only for directories this same
 * operation created and then identified. No marker field chooses a write path. */
class RootLease {
	const CloudBackupRoots &roots;
	QJsonObject snapshot;
	QHash<QString, QString> created;
public:
	explicit RootLease(const CloudBackupRoots &value) : roots(value) {}
	bool Capture(QString &error)
	{
		QString identity;
		return RootBinding(roots, identity, error, &snapshot, true);
	}
	bool Validate(QString &error) const
	{
		for (const auto &entry : RootPaths(roots)) {
			const auto expected = snapshot.value("roots").toObject().value(entry.first).toObject();
			QString resolved, object;
			if (expected.value("path").toString() != entry.second ||
			    !PathIdentity(entry.second, resolved, error) || resolved != expected.value("resolved").toString() ||
			    !DirectoryObject(expected.value("anchor").toString(), object, error) || object != expected.value("object").toString()) {
				if (error.isEmpty()) error = QObject::tr("Um diretório ou volume local mudou durante a operação. A restauração foi preservada.");
				return false;
			}
			QString cursor = expected.value("anchor").toString();
			for (const auto &part : expected.value("missing").toArray()) {
				cursor = QDir(cursor).filePath(part.toString());
				if (!QFileInfo::exists(cursor)) continue;
				if (!created.contains(cursor) || !DirectoryObject(cursor, object, error) || created.value(cursor) != object) {
					error = QObject::tr("Uma raiz anteriormente ausente apareceu fora desta tentativa. Preserve a restauração e confirme o destino novamente."); return false;
				}
			}
		}
		return true;
	}
	bool EnsureDirectory(const QString &path, QString &error)
	{
		if (!Validate(error)) return false;
		QString resolved;
		if (!PathIdentity(path, resolved, error)) return false;
		QString cursor = path;
		QStringList missing;
		while (!QFileInfo::exists(cursor)) {
			const QFileInfo info(cursor);
			missing.prepend(info.fileName()); cursor = info.absolutePath();
		}
		if (!QFileInfo(cursor).isDir()) { error = QObject::tr("Um ancestral do destino não é diretório."); return false; }
		for (const QString &part : missing) {
			cursor = QDir(cursor).filePath(part);
			if (!QDir().mkdir(cursor)) { error = QObject::tr("Não foi possível criar o diretório próprio da tentativa."); return false; }
			QString object;
			if (!DirectoryObject(cursor, object, error)) return false;
			created.insert(cursor, object);
		}
		return Validate(error);
	}
};

/* GUI capture/current-context comparison does no filesystem I/O. Canonical
 * alias checks and local persistent binding are computed only by the worker or
 * startup operation, from these captured strings rather than App(). */
bool RootIdentity(const CloudBackupRoots &roots, QString &identity, QString &error)
{
	const QStringList paths{roots.configRoot, roots.globalConfigPath, roots.profilesRoot,
		roots.scenesRoot, roots.pluginConfigRoot, roots.pluginManagerRoot, roots.pendingRoot,
		roots.safetyRoot, roots.coordinationRoot};
	QJsonArray values;
	for (const QString &path : paths) {
		if (path.isEmpty() || !QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path || path.contains(QChar('\0'))) {
			error = QObject::tr("Uma raiz local não é absoluta ou está inválida."); return false;
		}
		values.append(path);
	}
	if (roots.obsVersion.isEmpty()) { error = QObject::tr("A versão local não foi identificada."); return false; }
	values.append(roots.obsVersion);
	identity = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(values).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
	return true;
}

bool ValidateRoots(const CloudBackupRoots &roots, QString &error)
{
	QString current;
	if (!RootIdentity(roots, current, error)) return false;
	if (roots.identity.isEmpty() || roots.identity != current) {
		error = QObject::tr("As raízes locais mudaram. A restauração foi preservada; prepare-a novamente no contexto correto.");
		return false;
	}
	QString binding;
	return RootBinding(roots, binding, error, nullptr, true); // Validate cache aliases before any mkdir/lock I/O.
}

bool Cancelled(const function<bool()> &cancelled, QString &error)
{
	if (!cancelled || !cancelled()) return false;
	error = QObject::tr("A operação local de backup foi cancelada antes da publicação.");
	return true;
}

class RestoreLock {
	QLockFile lock;
public:
	explicit RestoreLock(const CloudBackupRoots &roots)
		: lock(QDir(roots.coordinationRoot).filePath("restore.lock")) {}
	bool Acquire(const CloudBackupRoots &roots, QString &error)
	{
		if (!ValidateRoots(roots, error)) return false;
		if (!QDir().mkpath(roots.coordinationRoot)) {
			error = QObject::tr("Não foi possível criar a área de coordenação da restauração."); return false;
		}
		lock.setStaleLockTime(0); // Never expire a slow live operation by elapsed wall time.
		if (!lock.tryLock(0)) {
			error = QObject::tr("Outra operação local de backup ou restauração está em andamento. Tente novamente quando terminar."); return false;
		}
		return true;
	}
};

class StageCleanup {
public:
	QString owned;
	~StageCleanup() { if (!owned.isEmpty()) QDir(owned).removeRecursively(); }
};

QString MarkerPath(const CloudBackupRoots &roots) { return QDir(roots.pendingRoot).filePath("pending.json"); }
QString LegacyPending(const CloudBackupRoots &roots) { return QDir(roots.pendingRoot).filePath("pending"); }
QString StagePath(const CloudBackupRoots &roots, const QString &id) { return QDir(roots.pendingRoot).filePath("stages/" + id); }

bool LegacyConflict(const CloudBackupRoots &roots, QString &error)
{
	if (!QFileInfo::exists(LegacyPending(roots))) return false;
	error = QObject::tr("Uma restauração pendente legada não identifica suas raízes. Ela foi preservada e precisa de recuperação manual antes de preparar outra.");
	return true;
}

QString BindingHash(const QJsonObject &binding)
{
	return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(binding).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

/* The marker never supplies a destination. Match each descriptor to its local
 * captured root, then inspect only ancestors constructed from that local path.
 * A missing root may have been created empty by normal startup; accept that
 * limited transition only on the same native volume. Nonempty/new-volume roots
 * require a new confirmation instead of silently adopting external contents. */
bool PublishedRootsCurrent(const CloudBackupRoots &roots, const QJsonObject &binding, QString &error)
{
	if (binding.value("bindingVersion").toInt() != 2 || binding.value("obsVersion").toString() != roots.obsVersion) {
		error = QObject::tr("A restauração pertence a outra versão local do aplicativo."); return false;
	}
	const auto expectedRoots = binding.value("roots").toObject();
	if (expectedRoots.size() != RootPaths(roots).size() - 1) { error = QObject::tr("O vínculo das raízes está incompleto."); return false; }
	for (const auto &entry : RootPaths(roots)) {
		if (entry.first == "coordination") continue;
		const auto expected = expectedRoots.value(entry.first).toObject();
		const bool fileRoot = entry.first == "global";
		QString resolved;
		if (expected.value("path").toString() != entry.second || expected.value("fileRoot").toBool() != fileRoot ||
		    !PathIdentity(entry.second, resolved, error) || expected.value("resolved").toString() != resolved) {
			if (error.isEmpty()) error = QObject::tr("A restauração pendente pertence a outras raízes locais."); return false;
		}
		const QString directory = fileRoot ? QFileInfo(entry.second).absolutePath() : entry.second;
		QString cursor = directory;
		QStringList suffix;
		while (cursor != expected.value("anchor").toString()) {
			const QFileInfo info(cursor);
			const auto parent = info.absolutePath();
			if (parent == cursor || info.fileName().isEmpty()) { error = QObject::tr("O ancestral do vínculo local é inválido."); return false; }
			suffix.prepend(info.fileName()); cursor = parent;
		}
		if (QJsonArray::fromStringList(suffix) != expected.value("missing").toArray()) {
			error = QObject::tr("O sufixo da raiz local é inválido."); return false;
		}
		QString object;
		if (!DirectoryObject(cursor, object, error) || object != expected.value("object").toString()) {
			if (error.isEmpty()) error = QObject::tr("O diretório ou volume original da restauração foi substituído."); return false;
		}
		const QString device = object.section(':', 0, 1);
		for (const QString &part : suffix) {
			cursor = QDir(cursor).filePath(part);
			if (!QFileInfo::exists(cursor)) continue;
			if (!DirectoryObject(cursor, object, error) || object.section(':', 0, 1) != device) {
				error = QObject::tr("Uma raiz anteriormente ausente mudou de volume ou tipo."); return false;
			}
		}
		if (!suffix.isEmpty() && QFileInfo::exists(directory) &&
		    !QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()) {
			error = QObject::tr("Uma raiz anteriormente ausente agora contém dados. A intenção foi preservada para nova confirmação."); return false;
		}
		if (fileRoot && QFileInfo::exists(entry.second) && !QFileInfo(entry.second).isFile()) {
			error = QObject::tr("A configuração global deixou de ser arquivo regular."); return false;
		}
	}
	return true;
}

bool LoadMarker(const CloudBackupRoots &roots, QJsonObject &marker, QString &stage, QString &error)
{
	QByteArray data;
	QString markerResolved;
	if (!PathIdentity(MarkerPath(roots), markerResolved, error) ||
	    !ReadFile(MarkerPath(roots), data, error)) return false;
	QJsonParseError parse;
	const auto document = QJsonDocument::fromJson(data, &parse);
	marker = document.object();
	const QString id = marker.value("stageId").toString();
	if (parse.error != QJsonParseError::NoError || !document.isObject() ||
	    marker.value("pendingVersion").toInt() != LOCAL_PENDING_VERSION ||
	    marker.value("rootBinding").toString() != BindingHash(marker.value("rootDescriptors").toObject()) ||
	    QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id ||
	    !marker.value("files").isArray() || marker.value("files").toArray().isEmpty()) {
		error = QObject::tr("A restauração pendente não corresponde às raízes e versão locais, ou tem manifesto inválido. Os dados foram preservados para recuperação.");
		return false;
	}
	if (!PublishedRootsCurrent(roots, marker.value("rootDescriptors").toObject(), error)) return false;
	stage = StagePath(roots, id);
	QString resolved;
	return PathIdentity(stage, resolved, error);
}

QList<SourceRoot> SelectedRoots(const CloudBackupRoots &roots, const CloudBackupOptions &options)
{
	QList<SourceRoot> selected;
	if (options.profiles) selected.push_back({"profiles", roots.profilesRoot});
	if (options.scenes) selected.push_back({"scenes", roots.scenesRoot});
	if (options.pluginSettings) {
		selected.push_back({"plugins/config", roots.pluginConfigRoot});
		selected.push_back({"plugins/manager", roots.pluginManagerRoot});
	}
	return selected;
}

bool SafeRelativePath(const QString &path)
{
	const QString clean = QDir::cleanPath(path);
	if (clean != path || path.isEmpty() || path == "." || path.contains(QChar('\0')) ||
	    QDir::isAbsolutePath(path) || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':')))
		return false;
	const QRegularExpression reserved(QStringLiteral("^(con|prn|aux|nul|conin\\$|conout\\$|clock\\$|com[1-9¹²³]|lpt[1-9¹²³])$"), QRegularExpression::CaseInsensitiveOption);
	for (const QString &part : path.split('/')) {
		if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ') ||
		    part.section('.', 0, 0).endsWith(' ') || reserved.match(part.section('.', 0, 0)).hasMatch()) return false;
		for (const QChar ch : part)
			if (ch.unicode() < 32 || QStringLiteral("<>\"|?*").contains(ch)) return false;
	}
	return true;
}

bool SameOrBelow(const QString &path, const QString &directory)
{
	const QString child = QDir::fromNativeSeparators(QDir::cleanPath(path)).toCaseFolded();
	const QString parent = QDir::fromNativeSeparators(QDir::cleanPath(directory)).toCaseFolded();
	return child == parent || child.startsWith(parent.endsWith('/') ? parent : parent + '/');
}

bool KeySameOrBelow(const QString &child, const QString &parent)
{
	return child == parent || child.startsWith(parent + '/');
}

QList<QString> InfrastructurePaths(const CloudBackupRoots &roots)
{
	return {roots.pendingRoot, roots.safetyRoot, roots.coordinationRoot};
}

QString DescriptorKey(const QJsonObject &descriptor)
{
	QString key = descriptor.value("object").toString();
	for (const auto &part : descriptor.value("missing").toArray()) key += '/' + part.toString().toCaseFolded();
	return key;
}

bool EffectiveTargetKey(const QString &target, QString &key, QString &error)
{
	QJsonObject parent;
	if (!RootDescriptor(QFileInfo(target).absolutePath(), false, parent, error)) return false;
	key = DescriptorKey(parent) + '/' + QFileInfo(target).fileName().toCaseFolded();
	return true;
}

bool DirectoryContains(const QString &directory, const QString &child, bool &contains, QString &error)
{
	contains = false;
	if (!QFileInfo::exists(directory)) return true;
	QString wanted;
	if (!DirectoryObject(directory, wanted, error)) return false;
	QString cursor = child;
	while (true) {
		if (QFileInfo::exists(cursor)) {
			QString object;
			if (!DirectoryObject(cursor, object, error)) return false;
			if (object == wanted) { contains = true; return true; }
		}
		const QString parent = QFileInfo(cursor).absolutePath();
		if (parent == cursor) return true;
		cursor = parent;
	}
}

bool RecursiveSourceSafe(const CloudBackupRoots &roots, const QString &source, QString &error)
{
	QJsonObject sourceDescriptor;
	if (!RootDescriptor(source, false, sourceDescriptor, error)) return false;
	for (const QString &infra : InfrastructurePaths(roots)) {
		QJsonObject infraDescriptor;
		if (!RootDescriptor(infra, false, infraDescriptor, error)) return false;
		bool sourceContains = false, infraContains = false;
		if (!DirectoryContains(source, infra, sourceContains, error) || !DirectoryContains(infra, source, infraContains, error)) return false;
		const QString sourceKey = DescriptorKey(sourceDescriptor), infraKey = DescriptorKey(infraDescriptor);
		if (SameOrBelow(source, infra) || SameOrBelow(infra, source) || sourceContains || infraContains ||
		    KeySameOrBelow(sourceKey, infraKey) || KeySameOrBelow(infraKey, sourceKey)) {
			error = QObject::tr("Uma categoria recursiva se sobrepõe à área interna de segurança ou restauração. Escolha raízes separadas antes de continuar."); return false;
		}
	}
	return true;
}

bool TargetOutsideInfrastructure(const CloudBackupRoots &roots, const QString &target, QString &error)
{
	QString targetKey;
	if (!EffectiveTargetKey(target, targetKey, error)) return false;
	for (const QString &infra : InfrastructurePaths(roots)) {
		QJsonObject descriptor;
		if (!RootDescriptor(infra, false, descriptor, error)) return false;
		bool inside = false;
		if (!DirectoryContains(infra, QFileInfo(target).absolutePath(), inside, error)) return false;
		const QString infraKey = DescriptorKey(descriptor);
		if (SameOrBelow(target, infra) || SameOrBelow(infra, target) || inside ||
		    KeySameOrBelow(targetKey, infraKey) || KeySameOrBelow(infraKey, targetKey)) {
			error = QObject::tr("Um arquivo do backup aponta para a área interna de segurança, coordenação ou restauração. A intenção anterior foi preservada."); return false;
		}
	}
	return true;
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

QByteArray MergeLocalDeviceBindings(const CloudBackupRoots &roots, const QString &archivePath, const QByteArray &incoming)
{
	const QString target = TargetPath(roots, archivePath);
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

QString TargetPath(const CloudBackupRoots &roots, const QString &archivePath)
{
	QString relative, root;
	if (archivePath == "general/user.ini") return QDir(roots.configRoot).filePath("user.ini");
	if (archivePath == "general/global.ini") return roots.globalConfigPath;
	if (archivePath.startsWith("profiles/")) { root = roots.profilesRoot; relative = archivePath.mid(9); }
	else if (archivePath.startsWith("scenes/")) { root = roots.scenesRoot; relative = archivePath.mid(7); }
	else if (archivePath.startsWith("plugins/config/")) { root = roots.pluginConfigRoot; relative = archivePath.mid(15); }
	else if (archivePath.startsWith("plugins/manager/")) { root = roots.pluginManagerRoot; relative = archivePath.mid(16); }
	return !root.isEmpty() && SafeRelativePath(relative) ? QDir(root).filePath(relative) : QString{};
}

bool ValidateTarget(const CloudBackupRoots &roots, const QString &path, QString &error)
{
	const QString target = TargetPath(roots, path);
	QString resolved;
	if (!target.isEmpty() && !TargetOutsideInfrastructure(roots, target, error)) return false;
	if (target.isEmpty() || !PathIdentity(target, resolved, error)) {
		if (error.isEmpty()) error = QObject::tr("O destino do arquivo %1 não é reconhecido.").arg(path);
		return false;
	}
	if (QFileInfo::exists(target) && !QFileInfo(target).isFile()) {
		error = QObject::tr("O destino do arquivo %1 não é um arquivo regular.").arg(path); return false;
	}
	return true;
}

QString DataHash(const QByteArray &data)
{
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool CheckStaged(const QString &path, const QJsonObject &item, const QString &prefix, QString &error)
{
	QString resolved;
	QByteArray data;
	if (!PathIdentity(path, resolved, error) || !ReadFile(path, data, error)) return false;
	const qint64 size = static_cast<qint64>(item.value(prefix + "Size").toDouble(-1));
	if (size < 0 || data.size() != size || DataHash(data) != item.value(prefix + "Sha256").toString()) {
		error = QObject::tr("Um arquivo preparado para restauração falhou na verificação de integridade."); return false;
	}
	return true;
}

bool SectionSelected(const QString &path, const CloudBackupOptions &options)
{
	return (path.startsWith(QStringLiteral("general/")) && options.generalSettings) ||
	       (path.startsWith(QStringLiteral("profiles/")) && options.profiles) ||
	       (path.startsWith(QStringLiteral("scenes/")) && options.scenes) ||
	       (path.startsWith(QStringLiteral("plugins/")) && options.pluginSettings);
}
} // namespace

static bool CreateSnapshot(const CloudBackupRoots &roots, const CloudBackupOptions &options, QByteArray &archive, CloudBackupSummary &summary,
				QString &error, const function<bool()> &cancelled, RootLease &lease)
{
	if (options.includeSecrets) { error = QObject::tr("O backup de credenciais está temporariamente indisponível até a validação do isolamento do cofre. Os arquivos podem ser copiados sem credenciais."); return false; }
	if (Cancelled(cancelled, error) || !lease.Validate(error)) return false;
	if (!options.generalSettings && !options.profiles && !options.scenes && !options.pluginSettings) {
		error = QObject::tr("Selecione pelo menos uma categoria para o backup.");
		return false;
	}
	if (options.includeSecrets && options.passphrase.size() < 8) {
		error = QObject::tr("Use uma senha com pelo menos 8 caracteres para incluir credenciais.");
		return false;
	}
	if (options.includeSecrets && !CloudBackupArchive::EncryptionSupported()) {
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
		QString resolved;
		if (Cancelled(cancelled, error) || !lease.Validate(error) || !PathIdentity(diskPath, resolved, error)) return false;
		QByteArray data;
		return ReadFile(diskPath, data, error) && addData(archivePath, std::move(data));
	};

	if (options.generalSettings) {
		const QString userIni = QDir(roots.configRoot).filePath(QStringLiteral("user.ini"));
		if (QFileInfo::exists(userIni) && !addFile(QStringLiteral("general/user.ini"), userIni))
			return false;
		const QString globalIni = roots.globalConfigPath;
		if (QFileInfo::exists(globalIni) && !addFile(QStringLiteral("general/global.ini"), globalIni))
			return false;
	}
	for (const SourceRoot &source : SelectedRoots(roots, options)) {
		if (!RecursiveSourceSafe(roots, source.path, error)) return false;
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
			if (!SafeRelativePath(relative)) { error = QObject::tr("Um arquivo local usa um nome inseguro ou incompatível com o backup portátil."); return false; }
			if (IsExcludedRelativePath(relative)) continue;
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
			 {QStringLiteral("obsVersion"), roots.obsVersion},
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
	summary = {ARCHIVE_FORMAT_VERSION, roots.obsVersion,
		   QSysInfo::machineHostName(), created, encrypt, static_cast<int>(files.size()), total};
	error.clear();
	return true;
}

bool CloudBackupArchive::CaptureRoots(CloudBackupRoots &roots, QString &error)
{
	if (!App()) { error = QObject::tr("O aplicativo não está disponível para capturar as raízes locais."); return false; }
	CloudBackupRoots captured;
	captured.configRoot = AppPath(App()->userConfigLocation, "obs-studio");
	captured.profilesRoot = AppPath(App()->userProfilesLocation, "obs-studio/basic/profiles");
	captured.scenesRoot = AppPath(App()->userScenesLocation, "obs-studio/basic/scenes");
	captured.pluginConfigRoot = QDir(captured.configRoot).filePath("plugin_config");
	captured.pluginManagerRoot = AppPath(App()->userPluginManagerSettingsLocation, "obs-studio/plugin_manager");
	captured.globalConfigPath = AppGlobalPath("obs-studio/global.ini");
	captured.pendingRoot = AppGlobalPath("obs-studio/cloud-restore");
	captured.safetyRoot = AppGlobalPath("obs-studio/backups/cloud");
	captured.coordinationRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
		.filePath("OtakudOBS/cloud-restore");
	captured.obsVersion = QString::fromStdString(App()->GetVersionString(false));
	if (!RootIdentity(captured, captured.identity, error)) return false;
	roots = std::move(captured);
	error.clear(); return true;
}

bool CloudBackupArchive::RootsCurrent(const CloudBackupRoots &roots, QString &error)
{
	CloudBackupRoots current;
	QString expected;
	if (!RootIdentity(roots, expected, error)) return false;
	if (expected != roots.identity) { error = QObject::tr("O snapshot das raízes locais foi alterado."); return false; }
	if (!CaptureRoots(current, error)) return false;
	if (current.identity != roots.identity) {
		error = QObject::tr("As raízes locais mudaram durante a operação. Nenhuma restauração foi redirecionada."); return false;
	}
	error.clear(); return true;
}

bool CloudBackupArchive::Create(const CloudBackupRoots &roots, const CloudBackupOptions &options, QByteArray &archive,
	CloudBackupSummary &summary, QString &error, const function<bool()> &cancelled)
{
	if (options.includeSecrets) { error = QObject::tr("O backup de credenciais está temporariamente indisponível. Continue com os arquivos sem credenciais."); return false; }
	RestoreLock lock(roots);
	if (Cancelled(cancelled, error) || !lock.Acquire(roots, error)) return false;
	RootLease lease(roots);
	if (!lease.Capture(error)) return false;
	return CreateSnapshot(roots, options, archive, summary, error, cancelled, lease);
}

bool CloudBackupArchive::Inspect(const QByteArray &archive, const QString &passphrase, CloudBackupSummary &summary,
				 QString &error)
{
	QJsonObject root;
	return Decode(archive, passphrase, root, summary, error);
}

bool CloudBackupArchive::StageRestore(const CloudBackupRoots &roots, const QByteArray &archive, const CloudBackupOptions &options,
	QString &safetyBackup, QString &error, const function<bool()> &cancelled)
{
	if (options.restoreCredentials) { error = QObject::tr("A restauração de credenciais está temporariamente indisponível até a validação do isolamento do cofre. A intenção anterior foi preservada."); return false; }
	RestoreLock lock(roots);
	if (Cancelled(cancelled, error) || !lock.Acquire(roots, error) || LegacyConflict(roots, error)) return false;
	RootLease lease(roots);
	if (!lease.Capture(error)) return false;
	QString oldStage;
	if (QFileInfo::exists(MarkerPath(roots))) {
		QJsonObject previous;
		if (!LoadMarker(roots, previous, oldStage, error)) return false;
		const QString recovery = QDir(oldStage).filePath("apply-active.json");
		if (QFileInfo::exists(recovery) || QFileInfo(recovery).isSymLink()) {
			error = QObject::tr("Uma aplicação anterior tem recuperação pendente. Seus arquivos foram preservados; recupere ou arquive essa tentativa antes de preparar outra."); return false;
		}
	}
	QJsonObject root;
	CloudBackupSummary incoming;
	if (!Decode(archive, options.passphrase, root, incoming, error))
		return false;

	CloudBackupOptions safetyOptions;
	safetyOptions.includeSecrets = options.restoreCredentials;
	safetyOptions.passphrase = options.passphrase;
	QByteArray safetyData;
	CloudBackupSummary safetySummary;
	if (!CreateSnapshot(roots, safetyOptions, safetyData, safetySummary, error, cancelled, lease))
		return false;
	const QString backupDir = roots.safetyRoot;
	if (!lease.EnsureDirectory(backupDir, error)) {
		error = QObject::tr("Não foi possível criar a pasta local de segurança.");
		return false;
	}
	safetyBackup = QDir(backupDir).filePath(
		QStringLiteral("antes-da-restauracao-%1-%2.obscloud")
			.arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")), QUuid::createUuid().toString(QUuid::WithoutBraces)));
	if (!WriteAtomic(safetyBackup, safetyData, error))
		return false;

	const QString stageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const QString pending = StagePath(roots, stageId);
	if (QFileInfo::exists(pending)) { error = QObject::tr("A área temporária desta tentativa já existe."); return false; }
	StageCleanup cleanup;
	if (!lease.EnsureDirectory(pending, error)) { if (error.isEmpty()) error = QObject::tr("Não foi possível preparar a restauração."); return false; }
	cleanup.owned = pending;

	QJsonArray pendingFiles;
	qint64 selectedBytes = 0;
	QByteArray pendingCredentials;
	QSet<QString> paths, targets;
	for (const QJsonValue &value : root.value(QStringLiteral("files")).toArray()) {
		if (Cancelled(cancelled, error)) return false;
		const QJsonObject item = value.toObject();
		const QString path = item.value(QStringLiteral("path")).toString();
		const bool protectedCredentials = path == QStringLiteral("protected/credentials.json");
		if (!SafeRelativePath(path)) { error = QObject::tr("O backup contém um caminho inseguro ou incompatível."); return false; }
		if (!SectionSelected(path, options) && !(protectedCredentials && options.restoreCredentials)) continue;
		if (paths.contains(path.toCaseFolded()) || (!protectedCredentials && !ValidateTarget(roots, path, error))) {
			if (error.isEmpty()) error = QObject::tr("O backup contém destinos repetidos."); return false;
		}
		paths.insert(path.toCaseFolded());
		if (!protectedCredentials) {
			QString key;
			if (!EffectiveTargetKey(TargetPath(roots, path), key, error)) return false;
			if (targets.contains(key)) { error = QObject::tr("Categorias do backup apontam para o mesmo arquivo local."); return false; }
			targets.insert(key);
		}
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
			QString readError;
			if (!ReadFile(TargetPath(roots, path), localGlobal, readError)) {
				error = QObject::tr("O arquivo global.ini local precisa estar legível para preservar seus diretórios. Reabra ou repare a configuração local e tente novamente. Detalhes: %1").arg(readError);
				return false;
			}
			data = PreserveIniSection(data, localGlobal, QStringLiteral("Locations"));
		}
		if (options.preserveLocalDevices && path.startsWith(QStringLiteral("scenes/")) &&
		    lower.endsWith(QStringLiteral(".json")))
			data = MergeLocalDeviceBindings(roots, path, data);
		QJsonObject pendingItem{{"path", path}, {"dataSize", static_cast<double>(data.size())}, {"dataSha256", DataHash(data)}};
		if (protectedCredentials) {
			pendingCredentials = std::move(data);
		} else {
			const QString staged = QDir(pending).filePath(QStringLiteral("files/") + path);
			if (!WriteAtomic(staged, data, error))
				return false;
			const QString target = TargetPath(roots, path);
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
				pendingItem.insert("rollbackSize", static_cast<double>(original.size()));
				pendingItem.insert("rollbackSha256", DataHash(original));
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
		for (qsizetype i = 0; i < pendingFiles.size(); ++i) {
			auto item = pendingFiles[i].toObject();
			if (item.value("path").toString() != "protected/credentials.json") continue;
			item.insert("dataSize", static_cast<double>(protectedCredentials.size()));
			item.insert("dataSha256", DataHash(protectedCredentials));
			item.insert("rollbackSize", static_cast<double>(protectedRollback.size()));
			item.insert("rollbackSha256", DataHash(protectedRollback));
			pendingFiles[i] = item;
		}
		pendingCredentials.fill('\0');
		rollbackCredentials.fill('\0');
	}
	QString binding;
	QJsonObject descriptors;
	if (!lease.Validate(error) || !RootBinding(roots, binding, error, &descriptors)) return false;
	const QJsonObject marker{{"pendingVersion", LOCAL_PENDING_VERSION}, {"rootBinding", binding},
		{"rootDescriptors", descriptors}, {"stageId", stageId}, {"createdUtc", incoming.createdUtc.toString(Qt::ISODate)}, {"files", pendingFiles}};
	if (Cancelled(cancelled, error) || !lease.Validate(error)) return false;
	if (!WriteAtomic(MarkerPath(roots), QJsonDocument(marker).toJson(QJsonDocument::Compact), error)) return false;
	cleanup.owned.clear(); // Published pointer now owns this complete stage.
	if (!oldStage.isEmpty() && oldStage != pending && !QDir(oldStage).removeRecursively())
		blog(LOG_WARNING, "Cloud restore published; previous stage cleanup remains pending");
	error.clear(); return true;
}

bool CloudBackupArchive::ApplyPendingRestore(const CloudBackupRoots &roots, QString &error)
{
	// No pending intent: leave normal startup and redirected homes untouched.
	QString capturedIdentity;
	if (!RootIdentity(roots, capturedIdentity, error)) return false;
	if (capturedIdentity != roots.identity) { error = QObject::tr("O snapshot das raízes locais foi alterado."); return false; }
	if (!HasPendingRestore(roots)) { error.clear(); return true; }
	RestoreLock lock(roots);
	if (!lock.Acquire(roots, error) || LegacyConflict(roots, error)) return false;
	if (!QFileInfo::exists(MarkerPath(roots))) { error.clear(); return true; }
	QJsonObject marker;
	QString pending;
	if (!LoadMarker(roots, marker, pending, error)) return false;
	RootLease lease(roots);
	if (!lease.Capture(error)) return false;
	const QJsonArray files = marker.value("files").toArray();
	for (const auto &value : files) {
		if (value.toObject().value("path").toString() == "protected/credentials.json") {
			error = QObject::tr("Esta intenção inclui credenciais, cuja restauração está temporariamente indisponível. Nenhum arquivo ou credencial foi alterado; a intenção foi preservada."); return false;
		}
	}
	const QString activePath = QDir(pending).filePath("apply-active.json");
	if (QFileInfo::exists(activePath) || QFileInfo(activePath).isSymLink()) {
		error = QObject::tr("Uma aplicação anterior foi interrompida ou falhou. A intenção e a cópia imediatamente anterior foram preservadas; recupere ou confirme novamente antes de aplicar."); return false;
	}
	QSet<QString> paths, targets;
	for (const QJsonValue &value : files) {
		const auto item = value.toObject();
		const QString path = item.value("path").toString();
		if (!SafeRelativePath(path) || paths.contains(path.toCaseFolded())) {
			error = QObject::tr("A restauração contém destinos inseguros ou repetidos."); return false;
		}
		paths.insert(path.toCaseFolded());
		if (path == "protected/credentials.json") {
			if (!CheckStaged(QDir(pending).filePath("protected/credentials.bin"), item, "data", error) ||
			    !CheckStaged(QDir(pending).filePath("rollback/credentials.bin"), item, "rollback", error)) return false;
		} else if (!item.value("hadOriginal").isBool()) {
			error = QObject::tr("O manifesto de restauração não identifica o estado original do destino."); return false;
		} else if (!ValidateTarget(roots, path, error) ||
			!CheckStaged(QDir(pending).filePath("files/" + path), item, "data", error) ||
			(item.value("hadOriginal").toBool() && !CheckStaged(QDir(pending).filePath("rollback/" + path), item, "rollback", error))) return false;
		if (path != "protected/credentials.json") {
			QString key;
			if (!EffectiveTargetKey(TargetPath(roots, path), key, error)) return false;
			if (targets.contains(key)) { error = QObject::tr("A restauração contém destinos efetivos repetidos."); return false; }
			targets.insert(key);
		}
	}
	/* Rollback must restore the state immediately before Apply, not the older
	 * state captured at Stage (normal shutdown may have changed these files). */
	const QString attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const QString attempt = QDir(pending).filePath("apply-" + attemptId);
	if (QFileInfo::exists(attempt)) { error = QObject::tr("A área de recuperação desta aplicação já existe."); return false; }
	StageCleanup prepared;
	if (!lease.EnsureDirectory(attempt, error)) return false;
	prepared.owned = attempt;
	QJsonArray freshFiles;
	qint64 freshBytes = 0;
	for (const auto &value : files) {
		const auto staged = value.toObject();
		const QString path = staged.value("path").toString();
		if (path == "protected/credentials.json") continue; // Explicitly blocked above.
		if (!lease.Validate(error) || !ValidateTarget(roots, path, error)) return false;
		const QString target = TargetPath(roots, path);
		const bool hadOriginal = QFileInfo::exists(target);
		QJsonObject fresh{{"path", path}, {"hadOriginal", hadOriginal},
			{"dataSize", staged.value("dataSize")}, {"dataSha256", staged.value("dataSha256")}};
		if (hadOriginal) {
			QByteArray original;
			if (!ReadFile(target, original, error)) return false;
			if (freshBytes > MAX_ARCHIVE_SOURCE_BYTES - original.size()) { error = QObject::tr("A recuperação imediatamente anterior à aplicação excede 512 MB."); return false; }
			freshBytes += original.size();
			if (!WriteAtomic(QDir(attempt).filePath("rollback/" + path), original, error)) return false;
			fresh.insert("rollbackSize", static_cast<double>(original.size()));
			fresh.insert("rollbackSha256", DataHash(original));
		}
		freshFiles.append(fresh);
	}
	QJsonObject journal{{"journalVersion", 1}, {"attemptId", attemptId}, {"state", "applying"},
		{"rootBinding", marker.value("rootBinding")}, {"files", freshFiles}};
	if (!lease.Validate(error) || !WriteAtomic(activePath, QJsonDocument(journal).toJson(QJsonDocument::Compact), error)) return false;
	prepared.owned.clear(); // Keep recovery bytes after any subsequent failure/crash.
	// Same immutable snapshot is used for validation, writes AND rollback.
	QJsonArray applied;
	auto rollback = [&]() {
		bool complete = true;
		for (qsizetype i = applied.size(); i > 0; --i) {
			const auto item = applied[i - 1].toObject();
			const QString path = item.value("path").toString();
			QString why;
			bool ok = lease.Validate(why) && ValidateTarget(roots, path, why);
			if (ok && item.value("hadOriginal").toBool()) {
				QByteArray original;
				ok = ReadFile(QDir(attempt).filePath("rollback/" + path), original, why);
				if (ok && (original.size() != static_cast<qint64>(item.value("rollbackSize").toDouble(-1)) || DataHash(original) != item.value("rollbackSha256").toString())) {
					ok = false; why = QObject::tr("A cópia imediatamente anterior à aplicação falhou na integridade.");
				}
				if (ok) ok = WriteAtomic(TargetPath(roots, path), original, why);
			} else if (ok && QFileInfo::exists(TargetPath(roots, path))) {
				ok = QFile::remove(TargetPath(roots, path));
				if (!ok) why = QObject::tr("Não foi possível remover um arquivo criado pela restauração.");
			}
			if (!ok) { complete = false; blog(LOG_ERROR, "Could not roll back restored file %s: %s", path.toUtf8().constData(), why.toUtf8().constData()); }
		}
		journal.insert("state", complete ? "failed-rolled-back" : "failed-incomplete-rollback");
		journal.insert("rollbackComplete", complete);
		QString journalError;
		if (!WriteAtomic(activePath, QJsonDocument(journal).toJson(QJsonDocument::Compact), journalError))
			blog(LOG_ERROR, "Could not update restore recovery journal: %s", journalError.toUtf8().constData());
		if (!complete) error += QObject::tr(" O rollback ficou incompleto; preserve a área pendente para recuperação.");
	};
	for (const auto &value : freshFiles) {
		const auto item = value.toObject();
		const QString path = item.value("path").toString();
		if (path == "protected/credentials.json") continue;
		QByteArray data;
		if (!lease.Validate(error) || !ValidateTarget(roots, path, error) ||
		    !ReadFile(QDir(pending).filePath("files/" + path), data, error)) { rollback(); return false; }
		// Verify exactly the byte array consumed, not a different preflight read.
		if (data.size() != static_cast<qint64>(item.value("dataSize").toDouble(-1)) || DataHash(data) != item.value("dataSha256").toString()) {
			error = QObject::tr("O arquivo preparado mudou depois da verificação inicial. Nenhum byte sem integridade foi aplicado."); rollback(); return false;
		}
		if (!lease.EnsureDirectory(QFileInfo(TargetPath(roots, path)).absolutePath(), error) ||
		    !WriteAtomic(TargetPath(roots, path), data, error)) { rollback(); return false; }
		applied.append(item);
	}
	for (const auto &value : files) {
		const auto item = value.toObject();
		if (item.value("path").toString() != "protected/credentials.json") continue;
		QByteArray protectedData, data;
		if (!lease.Validate(error) ||
		    !ReadFile(QDir(pending).filePath("protected/credentials.bin"), protectedData, error) ||
		    !UnprotectLocalData(protectedData, data, error) || !RestoreProtectedCredentials(data, error)) {
			QByteArray encryptedRollback, plainRollback;
			QString why;
			if (!ReadFile(QDir(pending).filePath("rollback/credentials.bin"), encryptedRollback, why) ||
			    !UnprotectLocalData(encryptedRollback, plainRollback, why) || !RollbackProtectedCredentials(plainRollback, why))
				error += QObject::tr(" O rollback das credenciais ficou incompleto; preserve a área pendente.");
			data.fill('\0'); plainRollback.fill('\0'); rollback(); return false;
		}
		data.fill('\0');
	}
	// Removing the pointer first avoids a crash leaving a live pointer to deleted files.
	if (!QFile::remove(MarkerPath(roots))) {
		error = QObject::tr("Os arquivos foram restaurados, mas não foi possível retirar a intenção pendente. Preserve a área local antes de reiniciar."); return false;
	}
	if (!QDir(pending).removeRecursively()) blog(LOG_WARNING, "Cloud restore applied; stage cleanup remains pending");
	error.clear(); return true;
}

bool CloudBackupArchive::EncryptionSupported()
{
	return CloudBackupCrypto::Supported();
}

bool CloudBackupArchive::HasPendingRestore(const CloudBackupRoots &roots)
{
	return QFileInfo::exists(MarkerPath(roots)) || QFileInfo::exists(LegacyPending(roots));
}

QString CloudBackupArchive::LocalBackupDirectory(const CloudBackupRoots &roots)
{
	return roots.safetyRoot;
}
