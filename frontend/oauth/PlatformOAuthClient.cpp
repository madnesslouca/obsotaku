/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "PlatformOAuthClient.hpp"

#include "OAuthHttpClient.hpp"
#include "OAuthPkce.hpp"

#include <json11.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>

using namespace std;
using namespace json11;

namespace {
int64_t CurrentUnixTime()
{
	return chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
}

string JoinScopes(const vector<string_view> &scopes)
{
	string result;
	for (const auto scope : scopes) {
		if (!result.empty())
			result += ' ';
		result += scope;
	}
	return result;
}

string Query(const OAuthHttpClient::Fields &fields)
{
	string result;
	for (const auto &[name, value] : fields) {
		const string encodedName = OAuthHttpClient::UrlEncode(name);
		const string encodedValue = OAuthHttpClient::UrlEncode(value);
		if (encodedName.empty() || (!value.empty() && encodedValue.empty()))
			return {};
		result += result.empty() ? '?' : '&';
		result += encodedName + '=' + encodedValue;
	}
	return result;
}

bool ParseJson(const OAuthHttpResponse &response, Json &json, string &error)
{
	string parseError;
	json = Json::parse(response.body, parseError);
	if (!parseError.empty() || !json.is_object()) {
		error = "The platform returned an invalid JSON response.";
		return false;
	}
	return true;
}

string ApiError(const Json &json, long statusCode)
{
	string message = json["error_description"].string_value();
	if (message.empty())
		message = json["message"].string_value();
	if (message.empty() && json["error"].is_string())
		message = json["error"].string_value();
	if (message.empty())
		message = "The platform request failed with HTTP status " + to_string(statusCode) + ".";
	return message;
}

bool ParseTokenResponse(const OAuthHttpResponse &response, const string &previousRefreshToken, OAuthTokenSet &tokens,
			string &error)
{
	Json json;
	if (!ParseJson(response, json, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(json, response.statusCode);
		return false;
	}

	OAuthTokenSet result;
	result.accessToken = json["access_token"].string_value();
	result.refreshToken = json["refresh_token"].string_value();
	if (result.refreshToken.empty())
		result.refreshToken = previousRefreshToken;
	result.tokenType = json["token_type"].string_value();
	if (result.tokenType.empty())
		result.tokenType = "Bearer";

	if (json["scope"].is_string()) {
		result.scope = json["scope"].string_value();
	} else if (json["scope"].is_array()) {
		for (const auto &scope : json["scope"].array_items()) {
			if (!result.scope.empty())
				result.scope += ' ';
			result.scope += scope.string_value();
		}
	}

	const int64_t now = CurrentUnixTime();
	const int64_t expiresIn = static_cast<int64_t>(json["expires_in"].number_value());
	const int64_t refreshExpiresIn = static_cast<int64_t>(json["refresh_expires_in"].number_value());
	result.expiresAt = expiresIn > 0 ? now + expiresIn : 0;
	result.refreshExpiresAt = refreshExpiresIn > 0 ? now + refreshExpiresIn : 0;
	if (result.accessToken.empty()) {
		error = "The platform token response did not contain an access token.";
		return false;
	}

	tokens = std::move(result);
	error.clear();
	return true;
}

string TokenEndpoint(StreamPlatform platform, const OAuthClientRegistration &registration)
{
	if (!registration.tokenExchangeEndpoint.empty())
		return registration.tokenExchangeEndpoint;
	return string(GetStreamPlatformInfo(platform).tokenEndpoint);
}

void AddProxyPlatform(StreamPlatform platform, const OAuthClientRegistration &registration,
		      OAuthHttpClient::Fields &fields)
{
	if (!registration.tokenExchangeEndpoint.empty())
		fields.emplace_back("platform", string(GetStreamPlatformInfo(platform).id));
}

void AddLocalClientSecret(const OAuthClientRegistration &registration, OAuthHttpClient::Fields &fields)
{
	if (registration.tokenExchangeEndpoint.empty() && !registration.clientSecret.empty())
		fields.emplace_back("client_secret", registration.clientSecret);
}

bool AuthorizedGet(const string &url, const OAuthTokenSet &tokens, const OAuthHttpClient::Headers &extraHeaders,
		   OAuthHttpResponse &response, string &error)
{
	OAuthHttpClient::Headers headers = extraHeaders;
	headers.emplace_back("Authorization: Bearer " + tokens.accessToken);
	return OAuthHttpClient::Get(url, headers, response, error);
}

bool ResolveYouTube(const OAuthTokenSet &tokens, ResolvedStreamIngest &ingest, string &error)
{
	OAuthHttpResponse response;
	if (!AuthorizedGet("https://www.googleapis.com/youtube/v3/liveStreams?part=cdn&mine=true&maxResults=50", tokens,
			   {}, response, error))
		return false;

	Json json;
	if (!ParseJson(response, json, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(json, response.statusCode);
		return false;
	}

	string server;
	string streamKey;
	for (const auto &item : json["items"].array_items()) {
		const Json info = item["cdn"]["ingestionInfo"];
		server = info["rtmpsIngestionAddress"].string_value();
		if (server.empty())
			server = info["ingestionAddress"].string_value();
		streamKey = info["streamName"].string_value();
		if (!server.empty() && !streamKey.empty())
			break;
	}
	if (server.empty() || streamKey.empty()) {
		error = "No reusable YouTube live stream with ingest credentials was found.";
		return false;
	}

	if (!AuthorizedGet("https://www.googleapis.com/youtube/v3/channels?part=id%2Csnippet&mine=true&maxResults=1",
			   tokens, {}, response, error))
		return false;
	if (!ParseJson(response, json, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(json, response.statusCode);
		return false;
	}
	const auto &channels = json["items"].array_items();
	if (channels.empty() || channels.front()["id"].string_value().empty()) {
		error = "YouTube returned no channel for the connected account.";
		return false;
	}

	const Json thumbnails = channels.front()["snippet"]["thumbnails"];
	string avatarUrl = thumbnails["medium"]["url"].string_value();
	if (avatarUrl.empty())
		avatarUrl = thumbnails["default"]["url"].string_value();

	ingest = {StreamPlatform::YouTube,
		  channels.front()["id"].string_value(),
		  channels.front()["snippet"]["title"].string_value(),
		  std::move(server),
		  std::move(streamKey),
		  std::move(avatarUrl)};
	error.clear();
	return true;
}

bool ResolveTwitch(const OAuthClientRegistration &registration, const OAuthTokenSet &tokens,
		   ResolvedStreamIngest &ingest, string &error)
{
	OAuthHttpResponse response;
	if (!OAuthHttpClient::Get("https://id.twitch.tv/oauth2/validate", {"Authorization: OAuth " + tokens.accessToken},
				  response, error))
		return false;

	Json validation;
	if (!ParseJson(response, validation, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(validation, response.statusCode);
		return false;
	}
	const string accountId = validation["user_id"].string_value();
	const string displayName = validation["login"].string_value();
	if (accountId.empty()) {
		error = "Twitch token validation returned no broadcaster id.";
		return false;
	}

	const string keyUrl = "https://api.twitch.tv/helix/streams/key?broadcaster_id=" +
			      OAuthHttpClient::UrlEncode(accountId);
	if (!AuthorizedGet(keyUrl, tokens, {"Client-Id: " + registration.clientId}, response, error))
		return false;
	Json keyJson;
	if (!ParseJson(response, keyJson, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(keyJson, response.statusCode);
		return false;
	}
	const auto &keyItems = keyJson["data"].array_items();
	const string streamKey = keyItems.empty() ? string{} : keyItems.front()["stream_key"].string_value();
	if (streamKey.empty()) {
		error = "Twitch returned no stream key for the connected broadcaster.";
		return false;
	}

	if (!OAuthHttpClient::Get("https://ingest.twitch.tv/ingests", {}, response, error))
		return false;
	Json ingestJson;
	if (!ParseJson(response, ingestJson, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(ingestJson, response.statusCode);
		return false;
	}

	/* Profile picture is a separate Helix call; a failure here must not stop
	 * the connection, so the avatar simply stays empty. */
	string avatarUrl;
	OAuthHttpResponse userResponse;
	string userError;
	if (AuthorizedGet("https://api.twitch.tv/helix/users?id=" + OAuthHttpClient::UrlEncode(accountId), tokens,
			  {"Client-Id: " + registration.clientId}, userResponse, userError)) {
		Json userJson;
		string parseError;
		if (ParseJson(userResponse, userJson, parseError) && userResponse.statusCode >= 200 &&
		    userResponse.statusCode < 300) {
			const auto &users = userJson["data"].array_items();
			if (!users.empty())
				avatarUrl = users.front()["profile_image_url"].string_value();
		}
	}

	for (const auto &item : ingestJson["ingests"].array_items()) {
		string server = item["url_template"].string_value();
		const size_t placeholder = server.find("{stream_key}");
		if (placeholder == string::npos)
			continue;
		server.erase(placeholder);
		if (!server.empty() && server.back() == '/')
			server.pop_back();
		ingest = {StreamPlatform::Twitch, accountId, displayName, std::move(server), streamKey,
			  std::move(avatarUrl)};
		error.clear();
		return true;
	}

	error = "Twitch returned no usable ingest server.";
	return false;
}

bool ResolveKick(const OAuthTokenSet &tokens, ResolvedStreamIngest &ingest, string &error)
{
	OAuthHttpResponse response;
	if (!AuthorizedGet("https://api.kick.com/public/v1/channels", tokens, {}, response, error))
		return false;
	Json json;
	if (!ParseJson(response, json, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(json, response.statusCode);
		return false;
	}

	const auto &items = json["data"].array_items();
	if (items.empty()) {
		error = "Kick returned no channel for the connected account.";
		return false;
	}
	const Json channel = items.front();
	const string server = channel["stream"]["url"].string_value();
	const string streamKey = channel["stream"]["key"].string_value();
	if (server.empty() || streamKey.empty()) {
		error = "Kick returned no ingest URL or stream key. Verify the streamkey:read permission.";
		return false;
	}

	ostringstream accountId;
	accountId << static_cast<int64_t>(channel["broadcaster_user_id"].number_value());
	ingest = {StreamPlatform::Kick,          accountId.str(), channel["slug"].string_value(), server, streamKey,
		  channel["banner_picture"].string_value()};
	error.clear();
	return true;
}
} // namespace

bool PlatformOAuthClient::CreateAuthorizationSession(StreamPlatform platform,
					      const OAuthClientRegistration &registration,
					      const string &redirectUri, OAuthAuthorizationSession &session,
					      string &error)
{
	const auto &info = GetStreamPlatformInfo(platform);
	if (info.oauthFlow != StreamOAuthFlow::AuthorizationCodePkce) {
		error = "The selected platform does not use the PKCE authorization flow.";
		return false;
	}
	if (registration.clientId.empty() || redirectUri.empty()) {
		error = "OAuth authorization requires a client id and redirect URI.";
		return false;
	}
	if (info.requiresBackendTokenExchange && registration.tokenExchangeEndpoint.empty() &&
	    registration.clientSecret.empty()) {
		error = "Kick requires either the secure token proxy or a locally protected development client secret.";
		return false;
	}

	const OAuthPkceData pkce = OAuthPkce::Generate();
	OAuthHttpClient::Fields fields = {
		{"response_type", "code"},
		{"client_id", registration.clientId},
		{"redirect_uri", redirectUri},
		{"scope", JoinScopes(info.scopes)},
		{"state", pkce.state},
		{"code_challenge", pkce.challenge},
		{"code_challenge_method", "S256"},
	};
	if (platform == StreamPlatform::YouTube) {
		fields.emplace_back("access_type", "offline");
		fields.emplace_back("include_granted_scopes", "true");
	}

	const string query = Query(fields);
	if (query.empty()) {
		error = "Could not encode the OAuth authorization URL.";
		return false;
	}
	session = {platform, string(info.authorizationEndpoint) + query, redirectUri, pkce.state, pkce.verifier};
	error.clear();
	return true;
}

bool PlatformOAuthClient::ExchangeAuthorizationCode(const OAuthClientRegistration &registration,
					     const OAuthAuthorizationSession &session, const string &code,
					     OAuthTokenSet &tokens, string &error)
{
	if (registration.clientId.empty() || code.empty() || session.codeVerifier.empty()) {
		error = "OAuth code exchange is missing required values.";
		return false;
	}

	OAuthHttpClient::Fields fields = {
		{"grant_type", "authorization_code"},
		{"client_id", registration.clientId},
		{"code", code},
		{"redirect_uri", session.redirectUri},
		{"code_verifier", session.codeVerifier},
	};
	AddProxyPlatform(session.platform, registration, fields);
	AddLocalClientSecret(registration, fields);
	OAuthHttpResponse response;
	if (!OAuthHttpClient::PostForm(TokenEndpoint(session.platform, registration), fields, {}, response, error))
		return false;
	return ParseTokenResponse(response, {}, tokens, error);
}

bool PlatformOAuthClient::StartDeviceAuthorization(StreamPlatform platform,
					    const OAuthClientRegistration &registration,
					    OAuthDeviceAuthorization &authorization, string &error)
{
	const auto &info = GetStreamPlatformInfo(platform);
	if (info.oauthFlow != StreamOAuthFlow::DeviceCode || info.deviceAuthorizationEndpoint.empty()) {
		error = "The selected platform does not support Device Code authorization.";
		return false;
	}
	if (registration.clientId.empty()) {
		error = "Device Code authorization requires a client id.";
		return false;
	}

	OAuthHttpResponse response;
	if (!OAuthHttpClient::PostForm(string(info.deviceAuthorizationEndpoint),
				       {{"client_id", registration.clientId}, {"scopes", JoinScopes(info.scopes)}}, {},
				       response, error))
		return false;
	Json json;
	if (!ParseJson(response, json, error))
		return false;
	if (response.statusCode < 200 || response.statusCode >= 300) {
		error = ApiError(json, response.statusCode);
		return false;
	}

	authorization.deviceCode = json["device_code"].string_value();
	authorization.userCode = json["user_code"].string_value();
	authorization.verificationUri = json["verification_uri"].string_value();
	authorization.expiresInSeconds = static_cast<int>(json["expires_in"].number_value());
	authorization.pollIntervalSeconds = static_cast<int>(json["interval"].number_value());
	if (authorization.pollIntervalSeconds <= 0)
		authorization.pollIntervalSeconds = 5;
	if (authorization.deviceCode.empty() || authorization.userCode.empty() || authorization.verificationUri.empty()) {
		error = "The Device Code response is incomplete.";
		return false;
	}
	error.clear();
	return true;
}

OAuthDevicePollStatus PlatformOAuthClient::PollDeviceAuthorization(StreamPlatform platform,
							    const OAuthClientRegistration &registration,
							    const OAuthDeviceAuthorization &authorization,
							    OAuthTokenSet &tokens, string &error)
{
	const auto &info = GetStreamPlatformInfo(platform);
	OAuthHttpResponse response;
	if (!OAuthHttpClient::PostForm(
		    string(info.tokenEndpoint),
		    {{"client_id", registration.clientId},
		     {"scopes", JoinScopes(info.scopes)},
		     {"device_code", authorization.deviceCode},
		     {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}},
		    {}, response, error))
		return OAuthDevicePollStatus::Error;

	Json json;
	if (!ParseJson(response, json, error))
		return OAuthDevicePollStatus::Error;
	if (response.statusCode >= 200 && response.statusCode < 300)
		return ParseTokenResponse(response, {}, tokens, error) ? OAuthDevicePollStatus::Authorized
									   : OAuthDevicePollStatus::Error;

	string status = json["message"].string_value();
	if (status.empty())
		status = json["error"].string_value();
	if (status == "authorization_pending") {
		error.clear();
		return OAuthDevicePollStatus::Pending;
	}
	if (status == "slow_down") {
		error.clear();
		return OAuthDevicePollStatus::SlowDown;
	}
	if (status == "expired_token" || status == "invalid device code") {
		error = "The device authorization code expired.";
		return OAuthDevicePollStatus::Expired;
	}
	if (status == "access_denied") {
		error = "The user denied device authorization.";
		return OAuthDevicePollStatus::Denied;
	}
	error = ApiError(json, response.statusCode);
	return OAuthDevicePollStatus::Error;
}

bool PlatformOAuthClient::RefreshTokens(StreamPlatform platform, const OAuthClientRegistration &registration,
					const string &redirectUri, const OAuthTokenSet &currentTokens,
					OAuthTokenSet &tokens, string &error)
{
	if (registration.clientId.empty() || currentTokens.refreshToken.empty()) {
		error = "OAuth token refresh requires a client id and refresh token.";
		return false;
	}
	if (GetStreamPlatformInfo(platform).requiresBackendTokenExchange &&
	    registration.tokenExchangeEndpoint.empty() && registration.clientSecret.empty()) {
		error = "Kick token refresh requires either the secure token proxy or the locally protected client secret.";
		return false;
	}

	OAuthHttpClient::Fields fields = {
		{"grant_type", "refresh_token"},
		{"client_id", registration.clientId},
		{"refresh_token", currentTokens.refreshToken},
	};
	if (!redirectUri.empty())
		fields.emplace_back("redirect_uri", redirectUri);
	AddProxyPlatform(platform, registration, fields);
	AddLocalClientSecret(registration, fields);

	OAuthHttpResponse response;
	if (!OAuthHttpClient::PostForm(TokenEndpoint(platform, registration), fields, {}, response, error))
		return false;
	return ParseTokenResponse(response, currentTokens.refreshToken, tokens, error);
}

bool PlatformOAuthClient::ResolveIngest(StreamPlatform platform, const OAuthClientRegistration &registration,
					const OAuthTokenSet &tokens, ResolvedStreamIngest &ingest, string &error)
{
	if (tokens.AccessTokenExpired()) {
		error = "The access token is missing or expired and must be refreshed before resolving ingest credentials.";
		return false;
	}
	if (platform == StreamPlatform::Twitch && registration.clientId.empty()) {
		error = "Twitch ingest resolution requires the registered client id.";
		return false;
	}

	switch (platform) {
	case StreamPlatform::YouTube:
		return ResolveYouTube(tokens, ingest, error);
	case StreamPlatform::Twitch:
		return ResolveTwitch(registration, tokens, ingest, error);
	case StreamPlatform::Kick:
		return ResolveKick(tokens, ingest, error);
	default:
		break;
	}
	error = "This platform does not resolve ingest credentials through OAuth.";
	return false;
}
