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

#include <json11.hpp>

#include <QUrl>

#include <algorithm>

using namespace std;
using namespace json11;

namespace {
constexpr const char *TOKEN_NAMESPACE = "google-drive-backup";
constexpr const char *TOKEN_ACCOUNT = "primary";
constexpr const char *CONFIG_SECTION = "GoogleDriveBackup";
constexpr const char *MIME_TYPE = "application/vnd.obs.cloud-backup";

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

OAuthClientRegistration GoogleDriveBackupClient::Registration()
{
	OAuthClientRegistration registration;
	registration.clientId = qEnvironmentVariable("OBS_GOOGLE_DRIVE_CLIENT_ID").toStdString();
	if (registration.clientId.empty()) {
		const char *configured = config_get_string(App()->GetUserConfig(), CONFIG_SECTION, "ClientId");
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

bool GoogleDriveBackupClient::HasConnection()
{
	string error;
	return OAuthTokenSet::LoadAs(TOKEN_NAMESPACE, TOKEN_ACCOUNT, error).has_value();
}

bool GoogleDriveBackupClient::StoreTokens(const OAuthTokenSet &tokens, QString &error)
{
	string nativeError;
	if (!tokens.SaveAs(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError)) {
		error = FromStd(nativeError);
		return false;
	}
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::Disconnect(QString &error)
{
	string nativeError;
	if (!OAuthTokenSet::RemoveAs(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError)) {
		error = FromStd(nativeError);
		return false;
	}
	config_remove_value(App()->GetUserConfig(), CONFIG_SECTION, "AccountName");
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::AccessToken(string &token, QString &error)
{
	string nativeError;
	auto stored = OAuthTokenSet::LoadAs(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError);
	if (!stored) {
		error = nativeError.empty() ? QObject::tr("Conecte uma conta do Google Drive primeiro.")
					  : FromStd(nativeError);
		return false;
	}
	if (stored->AccessTokenExpired()) {
		OAuthTokenSet refreshed;
		if (!PlatformOAuthClient::RefreshTokens(StreamPlatform::YouTube, Registration(), {}, *stored, refreshed,
						      nativeError) ||
		    !refreshed.SaveAs(TOKEN_NAMESPACE, TOKEN_ACCOUNT, nativeError)) {
			error = FromStd(nativeError);
			return false;
		}
		stored = std::move(refreshed);
	}
	token = stored->accessToken;
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::AccountName(QString &name, QString &error)
{
	string token;
	if (!AccessToken(token, error))
		return false;
	OAuthHttpResponse response;
	string nativeError;
	if (!OAuthHttpClient::Get("https://www.googleapis.com/drive/v3/about?fields=user(displayName,emailAddress)",
				  Bearer(token), response, nativeError)) {
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
	config_set_string(App()->GetUserConfig(), CONFIG_SECTION, "AccountName", name.toUtf8().constData());
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	return true;
}

bool GoogleDriveBackupClient::List(vector<GoogleDriveBackupItem> &items, QString &error)
{
	string token;
	if (!AccessToken(token, error))
		return false;
	const string query = OAuthHttpClient::UrlEncode("trashed=false and 'appDataFolder' in parents");
	items.clear();
	string pageToken;
	do {
		string url = "https://www.googleapis.com/drive/v3/files?spaces=appDataFolder&pageSize=1000&orderBy="
			     "createdTime%20desc&fields=nextPageToken,files(id,name,size,createdTime,appProperties)&q=" +
			     query;
		if (!pageToken.empty())
			url += "&pageToken=" + OAuthHttpClient::UrlEncode(pageToken);
		OAuthHttpResponse response;
		string nativeError;
		if (!OAuthHttpClient::Get(url, Bearer(token), response, nativeError)) {
			error = FromStd(nativeError);
			return false;
		}
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
				     GoogleDriveBackupItem &item, QString &error)
{
	string token;
	if (!AccessToken(token, error))
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
	if (!OAuthHttpClient::SendJson("POST",
				       "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&fields="
				       "id,name,size,createdTime,appProperties",
				       metadata.dump(), headers, response, nativeError)) {
		error = FromStd(nativeError);
		return false;
	}
	if (!Success(response.statusCode) || response.location.empty()) {
		error = !Success(response.statusCode) ? ApiError(response)
						  : QObject::tr("O Google Drive não retornou a sessão de upload.");
		return false;
	}
	if (!OAuthHttpClient::SendBytes("PUT", response.location,
					string(archive.constData(), static_cast<size_t>(archive.size())), MIME_TYPE, {},
					response, nativeError)) {
		error = FromStd(nativeError);
		return false;
	}
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

bool GoogleDriveBackupClient::Download(const QString &fileId, QByteArray &archive, QString &error)
{
	if (fileId.isEmpty()) {
		error = QObject::tr("Selecione um backup para restaurar.");
		return false;
	}
	string token;
	if (!AccessToken(token, error))
		return false;
	OAuthHttpResponse response;
	string nativeError;
	const string url = "https://www.googleapis.com/drive/v3/files/" +
			   OAuthHttpClient::UrlEncode(fileId.toStdString()) + "?alt=media";
	if (!OAuthHttpClient::Get(url, Bearer(token), response, nativeError)) {
		error = FromStd(nativeError);
		return false;
	}
	if (!Success(response.statusCode)) {
		error = ApiError(response);
		return false;
	}
	archive = QByteArray(response.body.data(), static_cast<qsizetype>(response.body.size()));
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::Remove(const QString &fileId, QString &error)
{
	string token;
	if (!AccessToken(token, error))
		return false;
	OAuthHttpResponse response;
	string nativeError;
	const string url = "https://www.googleapis.com/drive/v3/files/" +
			   OAuthHttpClient::UrlEncode(fileId.toStdString());
	if (!OAuthHttpClient::SendBytes("DELETE", url, {}, {}, Bearer(token), response, nativeError)) {
		error = FromStd(nativeError);
		return false;
	}
	if (!Success(response.statusCode)) {
		error = ApiError(response);
		return false;
	}
	error.clear();
	return true;
}

bool GoogleDriveBackupClient::Prune(int retainCount, QString &error)
{
	vector<GoogleDriveBackupItem> items;
	if (!List(items, error))
		return false;
	retainCount = max(1, retainCount);
	for (size_t index = static_cast<size_t>(retainCount); index < items.size(); ++index) {
		if (!Remove(items[index].id, error))
			return false;
	}
	error.clear();
	return true;
}
