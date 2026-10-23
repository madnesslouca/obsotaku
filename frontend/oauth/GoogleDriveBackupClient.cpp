/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "GoogleDriveBackupClient.hpp"
#include "OAuthHttpClient.hpp"
#include "ui-config.h"

#include <OBSApp.hpp>

#include <util/config-file.h>
#include <util/base.h>

#include <json11.hpp>

#include <QUrl>
#include <QCryptographicHash>
#include <cassert>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

using namespace std;
using namespace json11;

namespace {
constexpr const char *TOKEN_NAMESPACE = "google-drive-backup";
constexpr const char *TOKEN_ACCOUNT = "primary";
constexpr const char *CONFIG_SECTION = "GoogleDriveBackup";
constexpr const char *MIME_TYPE = "application/vnd.obs.cloud-backup";
/* The backup account has its own credential namespace. Serialize read/refresh/
 * durable-save with deletion so rollback cannot restore a pre-rotation token. */
mutex credentialMutex;
atomic_uint64_t credentialEpoch{1};
QByteArray credentialHash;
bool credentialKnown = false;
QByteArray HashRaw(const optional<string> &raw)
{
	if (!raw)
		return QByteArray("missing");
	QCryptographicHash hash(QCryptographicHash::Sha256);
	hash.addData(QByteArrayView(raw->data(), static_cast<qsizetype>(raw->size())));
	return hash.result();
}
void ObserveRaw(const optional<string> &raw)
{
	const auto hash = HashRaw(raw);
	if (credentialKnown && hash != credentialHash)
		++credentialEpoch;
	credentialHash = hash;
	credentialKnown = true;
}
void RecordOwnRaw(const optional<string> &raw, bool newSession)
{
	credentialHash = HashRaw(raw);
	credentialKnown = true;
	if (newSession)
		++credentialEpoch;
}

bool MayContinue(const shared_ptr<SecureTokenStore::RequestContext> &request, QString &error)
{
	if (request->IsCancelled()) {
		error = QObject::tr("A operação do cofre foi cancelada.");
		return false;
	}
	if (chrono::steady_clock::now() >= request->GetDeadline()) {
		error = QObject::tr("A operação do cofre excedeu o prazo.");
		return false;
	}
	return true;
}

bool AcquireCredentialLock(unique_lock<mutex> &lock, const shared_ptr<SecureTokenStore::RequestContext> &request,
			   QString &error)
{
	while (MayContinue(request, error)) {
		if (lock.try_lock())
			return MayContinue(request, error);
		this_thread::sleep_until(min(request->GetDeadline(), chrono::steady_clock::now() + chrono::milliseconds(10)));
	}
	return false;
}

void ClearSecret(optional<string> &secret)
{
	if (!secret)
		return;
	volatile char *bytes = secret->empty() ? nullptr : secret->data();
	for (size_t i = 0; i < secret->size(); ++i)
		bytes[i] = 0;
	secret.reset();
}

struct SecretWipe {
	optional<string> &secret;
	~SecretWipe() { ClearSecret(secret); }
};

optional<OAuthTokenSet> LoadTokens(string &error)
{
	auto raw = SecureTokenStore::Load(TOKEN_NAMESPACE, TOKEN_ACCOUNT, error);
	SecretWipe wipe{raw};
	if (!error.empty())
		return nullopt;
	ObserveRaw(raw);
	return raw ? OAuthTokenSet::Parse(*raw, error) : nullopt;
}


bool SessionStillCurrent(const GoogleDriveBackupClient::SessionInfo &effective, QString &error)
{
	if (GoogleDriveBackupClient::SessionCurrent(effective)) return true;
	error = QObject::tr("A conexão mudou durante a solicitação; o resultado remoto não foi revertido.");
	return false;
}

QString FromStd(const string &value)
{
	return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

bool Success(long status)
{
	return status >= 200 && status < 300;
}

QString ApiError(const OAuthHttpResponse &response)
{
	string parseError;
	const Json json = Json::parse(response.body, parseError);
	string message;
	if (parseError.empty()) {
		message = json["error"]["message"].string_value();
		if (message.empty())
			message = json["error_description"].string_value();
	}
	if (message.empty())
		message = "Google Drive request failed with HTTP status " + to_string(response.statusCode) + ".";
	return FromStd(message);
}

bool ParseObject(const OAuthHttpResponse &response, Json &json, QString &error)
{
	string parseError;
	json = Json::parse(response.body, parseError);
	if (!parseError.empty() || !json.is_object()) {
		error = QObject::tr("O Google Drive retornou uma resposta inválida.");
		return false;
	}
	if (!Success(response.statusCode)) {
		error = ApiError(response);
		return false;
	}
	return true;
}

OAuthHttpClient::Headers Bearer(const string &token)
{
	return {"Authorization: Bearer " + token};
}
} // namespace

bool GoogleDriveBackupClient::ValidateSession(const SessionInfo &expected, QString &error,
	shared_ptr<SecureTokenStore::RequestContext> request)
{
	SessionInfo current;
	if (!HasConnection(error, std::move(request), &current)) {
		if (error.isEmpty()) error = QObject::tr("Conecte uma conta do Google Drive primeiro.");
		return false;
	}
	if (current.epoch != expected.epoch) {
		error = QObject::tr("A conta do Google Drive mudou. Atualize a conexão.");
		return false;
	}
	return true;
}

function<bool()> GoogleDriveBackupClient::CaptureConfigurationGuard(ConfigurationPart part)
{
	auto snapshot = [part] {
		using Snapshot = optional<vector<optional<string>>>;
		if (!App()) return Snapshot{};
		config_t *config = App()->GetUserConfig();
		if (!config) return Snapshot{};
		vector<optional<string>> fields;
		fields.emplace_back(App()->userConfigLocation.lexically_normal().u8string());
		const char *profile = config_get_string(config, "Basic", "ProfileDir");
		fields.emplace_back(profile ? profile : "");
		auto field = [&](const char *key) {
			const char *value = config_get_string(config, CONFIG_SECTION, key);
			fields.emplace_back(config_has_user_value(config, CONFIG_SECTION, key)
				? optional<string>(value ? value : "") : nullopt);
		};
		if (part != ConfigurationPart::Settings) {
			for (const char *key : {"ClientId", "AccountName"}) field(key);
			const auto registration = Registration();
			fields.emplace_back(registration.clientId);
			fields.emplace_back(registration.tokenExchangeEndpoint);
		}
		if (part != ConfigurationPart::Connection)
			for (const char *key : {"IncludeGeneral", "IncludeProfiles", "IncludeScenes", "IncludePlugins",
				"AutoEnabled", "IntervalDays", "Retention"}) field(key);
		if (part == ConfigurationPart::All) field("LastBackupUnix");
		return Snapshot{std::move(fields)};
	};
	return [snapshot, expected = snapshot()] {
		const auto current = snapshot();
		return expected && current && current == expected;
	};
}

OAuthClientRegistration GoogleDriveBackupClient::Registration()
{
	OAuthClientRegistration registration;
	registration.clientId = qEnvironmentVariable("OBS_GOOGLE_DRIVE_CLIENT_ID").toStdString();
	if (registration.clientId.empty()) {
		config_t *config = App() ? App()->GetUserConfig() : nullptr;
		const char *configured = config ? config_get_string(config, CONFIG_SECTION, "ClientId") : nullptr;
		if (configured)
			registration.clientId = configured;
	}
	if (registration.clientId.empty())
		registration.clientId = PRODUCT_GOOGLE_DRIVE_CLIENT_ID;
	if (registration.clientId.empty())
		registration.clientId = PRODUCT_YOUTUBE_CLIENT_ID;
	return registration;
}

bool GoogleDriveBackupClient::RegistrationReady()
{
	return !Registration().clientId.empty();
}

bool GoogleDriveBackupClient::HasConnection(QString &error, shared_ptr<SecureTokenStore::RequestContext> request, SessionInfo *session)
{
	if (!request)
		request = make_shared<SecureTokenStore::RequestContext>();
	unique_lock lock(credentialMutex, defer_lock);
	if (!AcquireCredentialLock(lock, request, error))
		return false;
	SecureTokenStore::RequestScope scope(request);
	string nativeError;
	const bool connected = LoadTokens(nativeError).has_value();
	error = FromStd(nativeError);
	if (session && error.isEmpty())
		*session = {credentialEpoch.load(), connected};
	return connected;
}

bool GoogleDriveBackupClient::StoreTokens(const OAuthTokenSet &tokens, QString &error)
{
	lock_guard lock(credentialMutex);
	string nativeError;
	auto raw = tokens.Serialize(nativeError);
	SecretWipe wipe{raw};
	if (!raw || !SecureTokenStore::Save(TOKEN_NAMESPACE, TOKEN_ACCOUNT, *raw, nativeError)) {
		error = FromStd(nativeError);
		return false;
	}
	RecordOwnRaw(raw, true);
	error.clear();
	return true;
}

uint64_t GoogleDriveBackupClient::SessionRevision()
{
	return credentialEpoch.load();
}

bool GoogleDriveBackupClient::SessionCurrent(const SessionInfo &session)
{
	return session.epoch != 0 && session.epoch == credentialEpoch.load();
}

struct GoogleDriveBackupClient::CredentialTransaction::Impl {
	unique_lock<mutex> lock{credentialMutex, defer_lock};
	shared_ptr<SecureTokenStore::RequestContext> request;
	optional<string> backup;
	optional<string> replacement;
	bool deleting = true;
	thread::id owner = this_thread::get_id();
	bool attempted = false;
	bool settled = false;
	~Impl() {
		assert(owner == this_thread::get_id());
		if (attempted && !settled)
			blog(LOG_WARNING, "Cloud credential transaction released without a confirmed decision.");
		ClearSecret(backup); ClearSecret(replacement);
	}
};
GoogleDriveBackupClient::CredentialTransaction::CredentialTransaction(unique_ptr<Impl> value) : state(std::move(value)) {}
GoogleDriveBackupClient::CredentialTransaction::~CredentialTransaction() = default;
unique_ptr<GoogleDriveBackupClient::CredentialTransaction> GoogleDriveBackupClient::BeginCredentialTransaction(
	const SessionInfo &expected, QString &error, shared_ptr<SecureTokenStore::RequestContext> request)
{
	auto state = make_unique<CredentialTransaction::Impl>();
	state->request = request ? std::move(request) : make_shared<SecureTokenStore::RequestContext>();
	if (!AcquireCredentialLock(state->lock, state->request, error))
		return {};
	SecureTokenStore::RequestScope scope(state->request);
	string nativeError;
	state->backup = SecureTokenStore::Load(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError);
	if (!nativeError.empty()) { error = FromStd(nativeError); return {}; }
	ObserveRaw(state->backup);
	if (expected.epoch && (!SessionCurrent(expected) || expected.connected != state->backup.has_value())) {
		error = QObject::tr("A conta do Google Drive mudou. Atualize a conexão antes de tentar novamente.");
		return {};
	}
	return unique_ptr<CredentialTransaction>(new CredentialTransaction(std::move(state)));
}
bool GoogleDriveBackupClient::CredentialTransaction::Remove(QString &error)
{
	assert(state->owner == this_thread::get_id());
	if (state->attempted || state->settled) {
		error = QObject::tr("A transação da credencial já foi utilizada."); return false;
	}
	if (!MayContinue(state->request, error)) return false;
	SecureTokenStore::RequestScope scope(state->request);
	string nativeError;
	state->attempted = true;
	const bool removed = SecureTokenStore::Remove(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError);
	if (removed && MayContinue(state->request, error)) { error.clear(); return true; }
	if (!nativeError.empty()) error = FromStd(nativeError);
	if (error.isEmpty()) error = QObject::tr("Não foi possível confirmar a remoção da credencial.");
	return false;
}
bool GoogleDriveBackupClient::CredentialTransaction::Store(const OAuthTokenSet &tokens, QString &error)
{
	assert(state->owner == this_thread::get_id());
	if (state->attempted || state->settled) {
		error = QObject::tr("A transação da credencial já foi utilizada."); return false;
	}
	if (!MayContinue(state->request, error)) return false;
	string nativeError;
	state->replacement = tokens.Serialize(nativeError);
	if (!state->replacement) { error = FromStd(nativeError); return false; }
	state->deleting = false;
	state->attempted = true;
	SecureTokenStore::RequestScope scope(state->request);
	const bool stored = SecureTokenStore::Save(TOKEN_NAMESPACE, TOKEN_ACCOUNT, *state->replacement, nativeError);
	if (stored && MayContinue(state->request, error)) { error.clear(); return true; }
	if (!nativeError.empty()) error = FromStd(nativeError);
	if (error.isEmpty()) error = QObject::tr("Não foi possível confirmar a gravação da credencial.");
	return false;
}
bool GoogleDriveBackupClient::CredentialTransaction::Rollback(QString &error)
{
	assert(state->owner == this_thread::get_id());
	if (state->settled || !state->attempted) return true;
	SecureTokenStore::RequestScope recovery(make_shared<SecureTokenStore::RequestContext>());
	string nativeError;
	auto current = SecureTokenStore::Load(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError);
	SecretWipe wipe{current};
	bool restored = false;
	if (nativeError.empty()) {
		if (current == state->backup) restored = true;
		else if ((state->deleting && !current) || (!state->deleting && current == state->replacement))
			restored = state->backup ? SecureTokenStore::Save(TOKEN_NAMESPACE, TOKEN_ACCOUNT, *state->backup, nativeError)
					 : SecureTokenStore::Remove(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError);
		else ObserveRaw(current); // Never replace an unexpected credential from another process.
	}
	if (!restored) {
		// The cached identity must not remain usable after an unconfirmed recovery.
		++credentialEpoch;
		credentialKnown = false;
		error += QObject::tr(" A restauração da credencial anterior não foi confirmada; consulte o estado novamente.");
	}
	state->settled = true;
	return restored;
}
GoogleDriveBackupClient::SessionInfo GoogleDriveBackupClient::CredentialTransaction::Commit()
{
	assert(state->owner == this_thread::get_id());
	assert(state->attempted && !state->settled);
	RecordOwnRaw(state->deleting ? nullopt : state->replacement, true);
	state->settled = true;
	ClearSecret(state->backup);
	ClearSecret(state->replacement);
	return {credentialEpoch.load(), !state->deleting};
}
bool GoogleDriveBackupClient::Disconnect(QString &error, shared_ptr<SecureTokenStore::RequestContext> request)
{
	auto lease = BeginCredentialTransaction({}, error, std::move(request));
	if (!lease) return false;
	if (lease->Remove(error)) { lease->Commit(); return true; }
	lease->Rollback(error);
	return false;
}

bool GoogleDriveBackupClient::AccessToken(string &token, QString &error, const OAuthClientRegistration &registration,
	const optional<SessionInfo> &expected, SessionInfo &effective)
{
	/* HTTP retains its existing timeouts. Do not wrap refresh plus its durable
	 * save in a UI request which could expire after remote token rotation. */
	lock_guard lock(credentialMutex);
	string nativeError;
	auto stored = LoadTokens(nativeError);
	if (expected && !SessionCurrent(*expected)) {
		error = QObject::tr("A conta do Google Drive mudou antes da solicitação. Atualize a conexão.");
		return false;
	}
	effective = {credentialEpoch.load(), stored.has_value()};
	if (!stored) {
		error = nativeError.empty() ? QObject::tr("Conecte uma conta do Google Drive primeiro.")
					  : FromStd(nativeError);
		return false;
	}
	if (stored->AccessTokenExpired()) {
		OAuthTokenSet refreshed;
		if (!PlatformOAuthClient::RefreshTokens(StreamPlatform::YouTube, registration, {}, *stored, refreshed,
						      nativeError)) {
			error = FromStd(nativeError);
			return false;
		}
		auto raw = refreshed.Serialize(nativeError);
		SecretWipe wipe{raw};
		if (!raw || !SecureTokenStore::Save(TOKEN_NAMESPACE, TOKEN_ACCOUNT, *raw, nativeError)) {
			error = FromStd(nativeError);
			return false;
		}
		RecordOwnRaw(raw, false);
		stored = std::move(refreshed);
	}
	token = stored->accessToken;
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::AccountName(QString &name, QString &error, const OAuthClientRegistration &registration,
				 const OAuthHttpClient::Cancellation &cancel, const optional<SessionInfo> &expected)
{
	if (cancel.IsCancelled()) {
		error = QObject::tr("Operação cancelada; o resultado remoto não foi confirmado.");
		return false;
	}
	string token;
	SessionInfo effective;
	if (!AccessToken(token, error, registration, expected, effective))
		return false;
	OAuthTokenSet issued;
	issued.accessToken = std::move(token);
	const bool success = AuthorizedAccountName(issued, name, error, cancel);
	if (success && !SessionCurrent(effective)) {
		error = QObject::tr("A conexão mudou durante a solicitação; atualize o estado.");
		return false;
	}
	return success;
}

bool GoogleDriveBackupClient::AuthorizedAccountName(const OAuthTokenSet &tokens, QString &name, QString &error,
	const OAuthHttpClient::Cancellation &cancel)
{
	if (cancel.IsCancelled() || tokens.accessToken.empty()) {
		error = QObject::tr("A autorização não está disponível para consultar a conta.");
		return false;
	}
	OAuthHttpResponse response;
	string nativeError;
	if (!OAuthHttpClient::Get("https://www.googleapis.com/drive/v3/about?fields=user(displayName,emailAddress)",
				  Bearer(tokens.accessToken), response, nativeError, cancel)) {
		error = FromStd(nativeError);
		return false;
	}
	Json json;
	if (!ParseObject(response, json, error))
		return false;
	const string display = json["user"]["displayName"].string_value();
	const string email = json["user"]["emailAddress"].string_value();
	name = FromStd(!email.empty() ? email : display);
	if (name.isEmpty())
		name = QObject::tr("Conta Google conectada");
	return true;
}

bool GoogleDriveBackupClient::List(vector<GoogleDriveBackupItem> &items, QString &error,
				 const OAuthClientRegistration &registration,
				 const OAuthHttpClient::Cancellation &cancel, const optional<SessionInfo> &expected)
{
	if (cancel.IsCancelled()) {
		error = QObject::tr("Operação cancelada; o resultado remoto não foi confirmado.");
		return false;
	}
	string token;
	SessionInfo effective;
	if (!AccessToken(token, error, registration, expected, effective))
		return false;
	const string query = OAuthHttpClient::UrlEncode("trashed=false and 'appDataFolder' in parents");
	items.clear();
	string pageToken;
	do {
		if (!SessionStillCurrent(effective, error)) return false;
		string url = "https://www.googleapis.com/drive/v3/files?spaces=appDataFolder&pageSize=1000&orderBy="
			     "createdTime%20desc&fields=nextPageToken,files(id,name,size,createdTime,appProperties)&q=" +
			     query;
		if (!pageToken.empty())
			url += "&pageToken=" + OAuthHttpClient::UrlEncode(pageToken);
		OAuthHttpResponse response;
		string nativeError;
		if (!OAuthHttpClient::Get(url, Bearer(token), response, nativeError, cancel)) {
			error = FromStd(nativeError);
			return false;
		}
		if (!SessionStillCurrent(effective, error)) return false;
		Json json;
		if (!ParseObject(response, json, error))
			return false;
		for (const Json &file : json["files"].array_items()) {
			const Json properties = file["appProperties"];
			if (properties["schema"].string_value() != "obs-cloud-2")
				continue;
			GoogleDriveBackupItem item;
			item.id = FromStd(file["id"].string_value());
			item.name = FromStd(file["name"].string_value());
			item.computerName = FromStd(properties["computer"].string_value());
			item.obsVersion = FromStd(properties["obsVersion"].string_value());
			item.createdUtc = QDateTime::fromString(FromStd(file["createdTime"].string_value()), Qt::ISODate);
			item.size = FromStd(file["size"].string_value()).toLongLong();
			item.encrypted = properties["encrypted"].string_value() == "true";
			if (!item.id.isEmpty())
				items.emplace_back(std::move(item));
		}
		const string nextPageToken = json["nextPageToken"].string_value();
		if (!nextPageToken.empty() && nextPageToken == pageToken) {
			error = QObject::tr("O Google Drive repetiu a página do histórico.");
			return false;
		}
		pageToken = nextPageToken;
	} while (!pageToken.empty());
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::Upload(const QByteArray &archive, const QString &computerName,
				     const QString &obsVersion, const QDateTime &createdUtc, bool encrypted,
				     GoogleDriveBackupItem &item, QString &error,
				     const OAuthClientRegistration &registration,
				 const OAuthHttpClient::Cancellation &cancel, const optional<SessionInfo> &expected)
{
	if (cancel.IsCancelled()) {
		error = QObject::tr("Operação cancelada; o resultado remoto não foi confirmado.");
		return false;
	}
	string token;
	SessionInfo effective;
	if (!AccessToken(token, error, registration, expected, effective))
		return false;
	const QString name = QStringLiteral("obs-backup-%1.obscloud")
				     .arg(createdUtc.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss")));
	const Json metadata = Json::object{
		{"name", name.toStdString()},
		{"mimeType", MIME_TYPE},
		{"parents", Json::array{"appDataFolder"}},
		{"appProperties",
		 Json::object{{"schema", "obs-cloud-2"},
			      {"computer", computerName.toStdString()},
			      {"obsVersion", obsVersion.toStdString()},
			      {"encrypted", encrypted ? "true" : "false"}}},
	};
	OAuthHttpResponse response;
	string nativeError;
	OAuthHttpClient::Headers headers = Bearer(token);
	headers.emplace_back("X-Upload-Content-Type: " + string(MIME_TYPE));
	headers.emplace_back("X-Upload-Content-Length: " + to_string(archive.size()));
	if (!SessionStillCurrent(effective, error)) return false;
	if (!OAuthHttpClient::SendJson("POST",
				       "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&fields="
				       "id,name,size,createdTime,appProperties",
				       metadata.dump(), headers, response, nativeError, cancel)) {
		error = FromStd(nativeError);
		return false;
	}
	if (!SessionStillCurrent(effective, error)) return false;
	if (!Success(response.statusCode) || response.location.empty()) {
		error = !Success(response.statusCode) ? ApiError(response)
						  : QObject::tr("O Google Drive não retornou a sessão de upload.");
		return false;
	}
	if (!OAuthHttpClient::SendBytes("PUT", response.location,
					string(archive.constData(), static_cast<size_t>(archive.size())), MIME_TYPE, {},
					response, nativeError, cancel)) {
		error = FromStd(nativeError);
		return false;
	}
	if (!SessionStillCurrent(effective, error)) return false;
	Json uploaded;
	if (!ParseObject(response, uploaded, error))
		return false;
	item.id = FromStd(uploaded["id"].string_value());
	item.name = name;
	item.computerName = computerName;
	item.obsVersion = obsVersion;
	item.createdUtc = createdUtc;
	item.size = archive.size();
	item.encrypted = encrypted;
	return true;
}

bool GoogleDriveBackupClient::Download(const QString &fileId, QByteArray &archive, QString &error,
				     const OAuthClientRegistration &registration,
				 const OAuthHttpClient::Cancellation &cancel, const optional<SessionInfo> &expected)
{
	if (fileId.isEmpty()) {
		error = QObject::tr("Selecione um backup para restaurar.");
		return false;
	}
	if (cancel.IsCancelled()) {
		error = QObject::tr("Operação cancelada; o resultado remoto não foi confirmado.");
		return false;
	}
	string token;
	SessionInfo effective;
	if (!AccessToken(token, error, registration, expected, effective))
		return false;
	OAuthHttpResponse response;
	string nativeError;
	const string url = "https://www.googleapis.com/drive/v3/files/" +
			   OAuthHttpClient::UrlEncode(fileId.toStdString()) + "?alt=media";
	if (!SessionStillCurrent(effective, error)) return false;
	if (!OAuthHttpClient::Get(url, Bearer(token), response, nativeError, cancel)) {
		error = FromStd(nativeError);
		return false;
	}
	if (!SessionStillCurrent(effective, error)) return false;
	if (!Success(response.statusCode)) {
		error = ApiError(response);
		return false;
	}
	archive = QByteArray(response.body.data(), static_cast<qsizetype>(response.body.size()));
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::Remove(const QString &fileId, QString &error, const OAuthClientRegistration &registration,
				 const OAuthHttpClient::Cancellation &cancel, const optional<SessionInfo> &expected)
{
	if (cancel.IsCancelled()) {
		error = QObject::tr("Operação cancelada; o resultado remoto não foi confirmado.");
		return false;
	}
	string token;
	SessionInfo effective;
	if (!AccessToken(token, error, registration, expected, effective))
		return false;
	OAuthHttpResponse response;
	string nativeError;
	const string url = "https://www.googleapis.com/drive/v3/files/" +
			   OAuthHttpClient::UrlEncode(fileId.toStdString());
	if (!SessionStillCurrent(effective, error)) return false;
	if (!OAuthHttpClient::SendBytes("DELETE", url, {}, {}, Bearer(token), response, nativeError, cancel)) {
		error = FromStd(nativeError);
		return false;
	}
	if (!SessionStillCurrent(effective, error)) return false;
	if (!Success(response.statusCode)) {
		error = ApiError(response);
		return false;
	}
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::Prune(int retainCount, QString &error, const OAuthClientRegistration &registration,
				 const OAuthHttpClient::Cancellation &cancel, const optional<SessionInfo> &expected)
{
	optional<SessionInfo> operation = expected;
	if (!operation) {
		SessionInfo session;
		if (!HasConnection(error, {}, &session)) {
			if (error.isEmpty()) error = QObject::tr("Conecte uma conta do Google Drive primeiro.");
			return false;
		}
		operation = session;
	}
	vector<GoogleDriveBackupItem> items;
	if (!List(items, error, registration, cancel, operation))
		return false;
	retainCount = max(1, retainCount);
	for (size_t index = static_cast<size_t>(retainCount); index < items.size(); ++index) {
		if (!Remove(items[index].id, error, registration, cancel, operation))
			return false;
	}
	error.clear();
	return true;
}
