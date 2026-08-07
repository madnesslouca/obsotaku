/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OAuthHttpClient.hpp"

#include <curl/curl.h>

#include <chrono>
#include <memory>
#include <thread>

using namespace std;

namespace {
constexpr int MAX_ATTEMPTS = 3;

bool IsLoopbackUrl(const string &url)
{
	const size_t schemeEnd = url.find("://");
	if (schemeEnd == string::npos)
		return false;

	const size_t hostStart = schemeEnd + 3;
	const size_t hostEnd = url.find_first_of(":/?#", hostStart);
	const string host = url.substr(hostStart, hostEnd == string::npos ? string::npos : hostEnd - hostStart);
	return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

bool ShouldRetry(const OAuthHttpResponse &response)
{
	return response.statusCode == 429 || (response.statusCode >= 500 && response.statusCode < 600);
}

struct CurlDeleter {
	void operator()(CURL *handle) const { curl_easy_cleanup(handle); }
};

struct HeaderDeleter {
	void operator()(curl_slist *headers) const { curl_slist_free_all(headers); }
};

using CurlHandle = unique_ptr<CURL, CurlDeleter>;
using CurlHeaders = unique_ptr<curl_slist, HeaderDeleter>;

size_t WriteResponse(char *data, size_t size, size_t count, void *userData)
{
	auto *body = static_cast<string *>(userData);
	const size_t bytes = size * count;
	body->append(data, bytes);
	return bytes;
}

bool Configure(CURL *curl, const string &url, const OAuthHttpClient::Headers &headers, OAuthHttpResponse &response,
	       CurlHeaders &nativeHeaders, string &error)
{
	/* Compare the parsed host, not the URL prefix: "http://127.0.0.1.evil.com"
	 * starts with the loopback prefix but resolves to a remote server. */
	if (url.rfind("http://", 0) == 0 && !IsLoopbackUrl(url)) {
		error = "OAuth requests to non-local endpoints must use HTTPS.";
		return false;
	}

	curl_slist *headerList = nullptr;
	for (const auto &header : headers) {
		curl_slist *updatedHeaders = curl_slist_append(headerList, header.c_str());
		if (!updatedHeaders) {
			curl_slist_free_all(headerList);
			error = "Could not allocate HTTP request headers.";
			return false;
		}
		headerList = updatedHeaders;
	}
	nativeHeaders.reset(headerList);

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nativeHeaders.get());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "OBS-Multistream/0.1");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	return true;
}

bool Perform(CURL *curl, OAuthHttpResponse &response, string &error)
{
	const CURLcode result = curl_easy_perform(curl);
	if (result != CURLE_OK) {
		error = string("OAuth HTTP request failed: ") + curl_easy_strerror(result) + ".";
		return false;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
	error.clear();
	return true;
}
} // namespace

bool OAuthHttpClient::Get(const string &url, const Headers &headers, OAuthHttpResponse &response, string &error)
{
	/* Platforms answer 429 and transient 5xx under load. Failing on the first
	 * one would mark a healthy account as permanently broken. */
	for (int attempt = 1;; ++attempt) {
		CurlHandle curl(curl_easy_init());
		if (!curl) {
			error = "Could not initialize the OAuth HTTP client.";
			return false;
		}

		response = {};
		CurlHeaders nativeHeaders;
		if (!Configure(curl.get(), url, headers, response, nativeHeaders, error))
			return false;
		if (!Perform(curl.get(), response, error))
			return false;
		if (attempt >= MAX_ATTEMPTS || !ShouldRetry(response))
			return true;
		this_thread::sleep_for(chrono::seconds(attempt));
	}
}

bool OAuthHttpClient::PostForm(const string &url, const Fields &fields, const Headers &headers,
			       OAuthHttpResponse &response, string &error)
{
	CurlHandle curl(curl_easy_init());
	if (!curl) {
		error = "Could not initialize the OAuth HTTP client.";
		return false;
	}

	string body;
	for (const auto &[name, value] : fields) {
		char *encodedName = curl_easy_escape(curl.get(), name.c_str(), static_cast<int>(name.size()));
		char *encodedValue = curl_easy_escape(curl.get(), value.c_str(), static_cast<int>(value.size()));
		if (!encodedName || !encodedValue) {
			if (encodedName)
				curl_free(encodedName);
			if (encodedValue)
				curl_free(encodedValue);
			error = "Could not encode the OAuth form request.";
			return false;
		}
		if (!body.empty())
			body += '&';
		body += encodedName;
		body += '=';
		body += encodedValue;
		curl_free(encodedName);
		curl_free(encodedValue);
	}

	Headers requestHeaders = headers;
	requestHeaders.emplace_back("Content-Type: application/x-www-form-urlencoded");

	for (int attempt = 1;; ++attempt) {
		response = {};
		CurlHeaders nativeHeaders;
		if (!Configure(curl.get(), url, requestHeaders, response, nativeHeaders, error))
			return false;
		curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
		curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
		curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
		if (!Perform(curl.get(), response, error))
			return false;
		if (attempt >= MAX_ATTEMPTS || !ShouldRetry(response))
			return true;
		this_thread::sleep_for(chrono::seconds(attempt));
	}
}

string OAuthHttpClient::UrlEncode(const string &value)
{
	CurlHandle curl(curl_easy_init());
	if (!curl)
		return {};
	char *encoded = curl_easy_escape(curl.get(), value.c_str(), static_cast<int>(value.size()));
	if (!encoded)
		return {};
	string result(encoded);
	curl_free(encoded);
	return result;
}
