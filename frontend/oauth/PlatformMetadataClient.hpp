/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "OAuthTokenSet.hpp"
#include "PlatformOAuthClient.hpp"

#include <string>
#include <vector>

/* What the user can change on a live channel. An empty field is left alone, so
 * a channel that only overrides its title does not clear its category. */
struct StreamMetadata {
	std::string title;
	std::string categoryId;
	std::string categoryName;
};

struct StreamCategory {
	std::string id;
	std::string name;
};

/* Title and category updates on the platforms whose API allows it, both before
 * and during a broadcast — the three of them accept changes while live, which
 * is how a streamer switches category mid-stream.
 *
 * Every call is blocking and belongs on MultistreamTaskPool, never on the UI
 * thread. */
class PlatformMetadataClient {
public:
	/* Applies what the metadata carries. accountId identifies the channel on
	 * the platform (broadcaster id, YouTube channel id). */
	static bool Update(StreamPlatform platform, const OAuthClientRegistration &registration,
			   const OAuthTokenSet &tokens, const std::string &accountId, const StreamMetadata &metadata,
			   std::string &error);

	/* Category lookup for the platforms that have one. Returns an empty list
	 * without error when the platform has no searchable catalog. */
	static bool SearchCategories(StreamPlatform platform, const OAuthClientRegistration &registration,
				     const OAuthTokenSet &tokens, const std::string &query,
				     std::vector<StreamCategory> &results, std::string &error);

	/* Whether the platform can search categories at all, so the interface can
	 * hide the field instead of offering a search that returns nothing. */
	static bool SupportsCategories(StreamPlatform platform);
};
