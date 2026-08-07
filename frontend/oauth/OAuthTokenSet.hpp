/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/StreamPlatform.hpp>

#include <cstdint>
#include <optional>
#include <string>

struct OAuthTokenSet {
	OAuthTokenSet() = default;
	OAuthTokenSet(const OAuthTokenSet &) = default;
	OAuthTokenSet(OAuthTokenSet &&) noexcept = default;
	OAuthTokenSet &operator=(const OAuthTokenSet &) = default;
	OAuthTokenSet &operator=(OAuthTokenSet &&) noexcept = default;
	~OAuthTokenSet();

	std::string accessToken;
	std::string refreshToken;
	std::string tokenType = "Bearer";
	std::string scope;
	int64_t expiresAt = 0;
	int64_t refreshExpiresAt = 0;

	bool AccessTokenExpired(int64_t skewSeconds = 60) const;
	void Clear();
	bool Save(StreamPlatform platform, const std::string &accountId, std::string &error) const;
	static std::optional<OAuthTokenSet> Load(StreamPlatform platform, const std::string &accountId,
						 std::string &error);
	static bool Remove(StreamPlatform platform, const std::string &accountId, std::string &error);
};
