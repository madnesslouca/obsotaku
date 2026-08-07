/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <string>
#include <utility>
#include <vector>

struct OAuthHttpResponse {
	long statusCode = 0;
	std::string body;
};

class OAuthHttpClient {
public:
	using Fields = std::vector<std::pair<std::string, std::string>>;
	using Headers = std::vector<std::string>;

	static bool Get(const std::string &url, const Headers &headers, OAuthHttpResponse &response, std::string &error);
	static bool PostForm(const std::string &url, const Fields &fields, const Headers &headers,
			     OAuthHttpResponse &response, std::string &error);
	static std::string UrlEncode(const std::string &value);
};
