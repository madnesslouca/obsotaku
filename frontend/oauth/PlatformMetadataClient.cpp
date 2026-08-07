/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "PlatformMetadataClient.hpp"

#include "OAuthHttpClient.hpp"

#include <json11.hpp>

using namespace std;
using namespace json11;

namespace {
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

string ApiError(const OAuthHttpResponse &response)
{
	string parseError;
	const Json json = Json::parse(response.body, parseError);
	if (parseError.empty() && json.is_object()) {
		string message = json["message"].string_value();
		if (message.empty())
			message = json["error"]["message"].string_value();
		if (message.empty())
			message = json["error_description"].string_value();
		if (!message.empty())
			return message;
	}
	return "The platform rejected the update with HTTP status " + to_string(response.statusCode) + ".";
}

bool Succeeded(const OAuthHttpResponse &response)
{
	return response.statusCode >= 200 && response.statusCode < 300;
}

OAuthHttpClient::Headers BearerHeaders(const OAuthTokenSet &tokens)
{
	return {"Authorization: Bearer " + tokens.accessToken};
}

OAuthHttpClient::Headers TwitchHeaders(const OAuthClientRegistration &registration, const OAuthTokenSet &tokens)
{
	return {"Authorization: Bearer " + tokens.accessToken, "Client-Id: " + registration.clientId};
}

bool UpdateTwitch(const OAuthClientRegistration &registration, const OAuthTokenSet &tokens, const string &accountId,
		  const StreamMetadata &metadata, string &error)
{
	Json::object body;
	if (!metadata.title.empty())
		body["title"] = metadata.title;
	if (!metadata.categoryId.empty())
		body["game_id"] = metadata.categoryId;
	if (body.empty()) {
		error.clear();
		return true;
	}

	const string url = "https://api.twitch.tv/helix/channels?broadcaster_id=" +
			   OAuthHttpClient::UrlEncode(accountId);
	OAuthHttpResponse response;
	if (!OAuthHttpClient::SendJson("PATCH", url, Json(body).dump(), TwitchHeaders(registration, tokens), response,
				       error))
		return false;
	/* Twitch answers 204 with no body when the change went through. */
	if (!Succeeded(response)) {
		error = ApiError(response);
		return false;
	}
	error.clear();
	return true;
}

bool UpdateKick(const OAuthTokenSet &tokens, const StreamMetadata &metadata, string &error)
{
	Json::object body;
	if (!metadata.title.empty())
		body["stream_title"] = metadata.title;
	if (!metadata.categoryId.empty()) {
		/* Kick takes the category as a number, not a string. */
		body["category_id"] = atoi(metadata.categoryId.c_str());
	}
	if (body.empty()) {
		error.clear();
		return true;
	}

	OAuthHttpResponse response;
	if (!OAuthHttpClient::SendJson("PATCH", "https://api.kick.com/public/v1/channels", Json(body).dump(),
				       BearerHeaders(tokens), response, error))
		return false;
	if (!Succeeded(response)) {
		error = ApiError(response);
		return false;
	}
	error.clear();
	return true;
}

/* YouTube edits the broadcast, so the active one has to be found first, and its
 * snippet resent whole: a partial update would clear the fields left out. */
bool UpdateYouTube(const OAuthTokenSet &tokens, const StreamMetadata &metadata, string &error)
{
	if (metadata.title.empty()) {
		error.clear();
		return true;
	}

	OAuthHttpResponse response;
	const string listUrl = "https://www.googleapis.com/youtube/v3/liveBroadcasts"
			       "?part=snippet&broadcastStatus=active&broadcastType=all";
	if (!OAuthHttpClient::Get(listUrl, BearerHeaders(tokens), response, error))
		return false;
	if (!Succeeded(response)) {
		error = ApiError(response);
		return false;
	}

	Json listJson;
	if (!ParseJson(response, listJson, error))
		return false;

	const auto &items = listJson["items"].array_items();
	if (items.empty()) {
		error = "YouTube has no active broadcast to update.";
		return false;
	}

	const Json broadcast = items.front();
	const string broadcastId = broadcast["id"].string_value();
	if (broadcastId.empty()) {
		error = "YouTube returned a broadcast without an id.";
		return false;
	}

	Json::object snippet;
	snippet["title"] = metadata.title;
	/* Preserved because the API rejects an update that drops them. */
	const string description = broadcast["snippet"]["description"].string_value();
	if (!description.empty())
		snippet["description"] = description;
	const string scheduledStart = broadcast["snippet"]["scheduledStartTime"].string_value();
	if (!scheduledStart.empty())
		snippet["scheduledStartTime"] = scheduledStart;

	Json::object body;
	body["id"] = broadcastId;
	body["snippet"] = snippet;

	if (!OAuthHttpClient::SendJson("PUT", "https://www.googleapis.com/youtube/v3/liveBroadcasts?part=snippet",
				       Json(body).dump(), BearerHeaders(tokens), response, error))
		return false;
	if (!Succeeded(response)) {
		error = ApiError(response);
		return false;
	}
	error.clear();
	return true;
}

bool SearchTwitchCategories(const OAuthClientRegistration &registration, const OAuthTokenSet &tokens,
			    const string &query, vector<StreamCategory> &results, string &error)
{
	const string url = "https://api.twitch.tv/helix/search/categories?first=20&query=" +
			   OAuthHttpClient::UrlEncode(query);
	OAuthHttpResponse response;
	if (!OAuthHttpClient::Get(url, TwitchHeaders(registration, tokens), response, error))
		return false;
	if (!Succeeded(response)) {
		error = ApiError(response);
		return false;
	}

	Json json;
	if (!ParseJson(response, json, error))
		return false;
	for (const auto &item : json["data"].array_items())
		results.push_back({item["id"].string_value(), item["name"].string_value()});
	error.clear();
	return true;
}

bool SearchKickCategories(const OAuthTokenSet &tokens, const string &query, vector<StreamCategory> &results,
			  string &error)
{
	const string url = "https://api.kick.com/public/v1/categories?q=" + OAuthHttpClient::UrlEncode(query);
	OAuthHttpResponse response;
	if (!OAuthHttpClient::Get(url, BearerHeaders(tokens), response, error))
		return false;
	if (!Succeeded(response)) {
		error = ApiError(response);
		return false;
	}

	Json json;
	if (!ParseJson(response, json, error))
		return false;
	for (const auto &item : json["data"].array_items()) {
		/* Kick returns the id as a number. */
		const string id = item["id"].is_number() ? to_string(static_cast<int64_t>(item["id"].number_value()))
							 : item["id"].string_value();
		results.push_back({id, item["name"].string_value()});
	}
	error.clear();
	return true;
}
} // namespace

bool PlatformMetadataClient::SupportsCategories(StreamPlatform platform)
{
	/* YouTube's categories belong to the video, not the broadcast, and are
	 * too coarse to be worth a search field here. */
	return platform == StreamPlatform::Twitch || platform == StreamPlatform::Kick;
}

bool PlatformMetadataClient::Update(StreamPlatform platform, const OAuthClientRegistration &registration,
				    const OAuthTokenSet &tokens, const string &accountId,
				    const StreamMetadata &metadata, string &error)
{
	if (!GetStreamPlatformInfo(platform).supportsMetadataUpdates) {
		error = "This platform does not expose an API for updating the broadcast.";
		return false;
	}
	if (tokens.AccessTokenExpired()) {
		error = "The access token is missing or expired and must be refreshed first.";
		return false;
	}

	switch (platform) {
	case StreamPlatform::Twitch:
		if (registration.clientId.empty()) {
			error = "Twitch updates require the registered client id.";
			return false;
		}
		if (accountId.empty()) {
			error = "Twitch updates require the broadcaster id.";
			return false;
		}
		return UpdateTwitch(registration, tokens, accountId, metadata, error);
	case StreamPlatform::Kick:
		return UpdateKick(tokens, metadata, error);
	case StreamPlatform::YouTube:
		return UpdateYouTube(tokens, metadata, error);
	default:
		break;
	}

	error = "This platform does not expose an API for updating the broadcast.";
	return false;
}

bool PlatformMetadataClient::SearchCategories(StreamPlatform platform, const OAuthClientRegistration &registration,
					      const OAuthTokenSet &tokens, const string &query,
					      vector<StreamCategory> &results, string &error)
{
	results.clear();
	if (!SupportsCategories(platform) || query.empty()) {
		error.clear();
		return true;
	}
	if (tokens.AccessTokenExpired()) {
		error = "The access token is missing or expired and must be refreshed first.";
		return false;
	}

	switch (platform) {
	case StreamPlatform::Twitch:
		return SearchTwitchCategories(registration, tokens, query, results, error);
	case StreamPlatform::Kick:
		return SearchKickCategories(tokens, query, results, error);
	default:
		break;
	}
	error.clear();
	return true;
}
