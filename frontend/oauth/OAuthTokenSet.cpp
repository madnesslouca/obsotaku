/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OAuthTokenSet.hpp"

#include <utility/SecureTokenStore.hpp>

#include <json11.hpp>

#include <chrono>

using namespace std;
using namespace json11;

static int64_t CurrentUnixTime()
{
	return chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
}

static void ClearSecret(string &value)
{
	volatile char *data = value.empty() ? nullptr : value.data();
	for (size_t index = 0; index < value.size(); ++index)
		data[index] = 0;
	value.clear();
}

OAuthTokenSet::~OAuthTokenSet()
{
	Clear();
}

void OAuthTokenSet::Clear()
{
	ClearSecret(accessToken);
	ClearSecret(refreshToken);
	tokenType.clear();
	scope.clear();
	expiresAt = 0;
	refreshExpiresAt = 0;
}

bool OAuthTokenSet::AccessTokenExpired(int64_t skewSeconds) const
{
	return accessToken.empty() || (expiresAt > 0 && CurrentUnixTime() + skewSeconds >= expiresAt);
}

bool OAuthTokenSet::Save(StreamPlatform platform, const string &accountId, string &error) const
{
	if (accessToken.empty() && refreshToken.empty()) {
		error = "An OAuth token set must contain an access or refresh token.";
		return false;
	}

	const Json json = Json::object{
		{"access_token", accessToken},
		{"refresh_token", refreshToken},
		{"token_type", tokenType},
		{"scope", scope},
		{"expires_at", static_cast<double>(expiresAt)},
		{"refresh_expires_at", static_cast<double>(refreshExpiresAt)},
	};
	return SecureTokenStore::Save(string(GetStreamPlatformInfo(platform).id), accountId, json.dump(), error);
}

optional<OAuthTokenSet> OAuthTokenSet::Load(StreamPlatform platform, const string &accountId, string &error)
{
	auto secret = SecureTokenStore::Load(string(GetStreamPlatformInfo(platform).id), accountId, error);
	if (!secret)
		return nullopt;

	string parseError;
	const Json json = Json::parse(*secret, parseError);
	secret->assign(secret->size(), '\0');
	if (!parseError.empty() || !json.is_object()) {
		error = "The stored OAuth credential is invalid.";
		return nullopt;
	}

	OAuthTokenSet result;
	result.accessToken = json["access_token"].string_value();
	result.refreshToken = json["refresh_token"].string_value();
	result.tokenType = json["token_type"].string_value();
	result.scope = json["scope"].string_value();
	result.expiresAt = static_cast<int64_t>(json["expires_at"].number_value());
	result.refreshExpiresAt = static_cast<int64_t>(json["refresh_expires_at"].number_value());
	if (result.accessToken.empty() && result.refreshToken.empty()) {
		error = "The stored OAuth credential contains no tokens.";
		return nullopt;
	}
	if (result.tokenType.empty())
		result.tokenType = "Bearer";
	error.clear();
	return result;
}

bool OAuthTokenSet::Remove(StreamPlatform platform, const string &accountId, string &error)
{
	return SecureTokenStore::Remove(string(GetStreamPlatformInfo(platform).id), accountId, error);
}
