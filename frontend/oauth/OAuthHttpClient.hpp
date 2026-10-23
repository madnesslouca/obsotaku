/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct OAuthHttpResponse {
	long statusCode = 0;
	std::string body;
	std::string location;
};

class OAuthHttpClient {
public:
	using Fields = std::vector<std::pair<std::string, std::string>>;
	using Headers = std::vector<std::string>;
	/* Opt-in only. Cancellation cannot prove that a remote mutation did not
	 * commit. Keep OAuth token exchange/rotation outside this scope. */
	struct Cancellation {
		std::shared_ptr<std::atomic_bool> operation;
		std::shared_ptr<std::atomic_bool> shutdown;
		bool IsCancelled() const noexcept
		{
			return (operation && operation->load()) || (shutdown && shutdown->load());
		}
		bool Enabled() const noexcept { return operation || shutdown; }
	};

	static bool Get(const std::string &url, const Headers &headers, OAuthHttpResponse &response, std::string &error,
			const Cancellation &cancel = {});
	static bool PostForm(const std::string &url, const Fields &fields, const Headers &headers,
			     OAuthHttpResponse &response, std::string &error);

	/* JSON body request for the platform APIs that update broadcast metadata.
	 * method is PATCH, PUT or POST. */
	static bool SendJson(const std::string &method, const std::string &url, const std::string &json,
			     const Headers &headers, OAuthHttpResponse &response, std::string &error,
			     const Cancellation &cancel = {});
	static bool SendBytes(const std::string &method, const std::string &url, const std::string &body,
			      const std::string &contentType, const Headers &headers, OAuthHttpResponse &response,
			      std::string &error, const Cancellation &cancel = {});

	static std::string UrlEncode(const std::string &value);
};
