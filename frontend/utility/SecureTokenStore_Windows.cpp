/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SecureTokenStore.hpp"

#include <windows.h>
#include <wincred.h>

#include <limits>

using namespace std;

static optional<wstring> Utf8ToWide(const string &value)
{
	if (value.empty())
		return wstring{};
	if (value.size() > static_cast<size_t>(numeric_limits<int>::max()))
		return nullopt;

	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
					 nullptr, 0);
	if (size <= 0)
		return nullopt;

	wstring result(static_cast<size_t>(size), L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
				 result.data(), size))
		return nullopt;
	return result;
}

static optional<wstring> CredentialTarget(const string &platformId, const string &accountId, string &error)
{
	if (platformId.empty() || accountId.empty() || platformId.find('\0') != string::npos ||
	    accountId.find('\0') != string::npos) {
		error = "Nonempty platform and account identifiers without NUL bytes are required for secure credential storage.";
		return nullopt;
	}

	auto target = Utf8ToWide("OBS-Multistream/OAuth/" + platformId + "/" + accountId);
	if (!target || target->size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH) {
		error = "The secure credential target name is invalid or too long.";
		return nullopt;
	}
	return target;
}

static string WindowsError(const char *operation, DWORD code)
{
	return string(operation) + " failed with Windows error " + to_string(code) + ".";
}

bool SecureTokenStore::Save(const string &platformId, const string &accountId, const string &secret, string &error)
{
	auto request = RequestForCurrentThread();
	if (!RequestMayStart(request, error))
		return false;
	auto target = CredentialTarget(platformId, accountId, error);
	if (!target) {
		request->status.store(Status::InvalidInput, memory_order_relaxed);
		return false;
	}
	if (secret.empty() || secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
		request->status.store(Status::InvalidInput, memory_order_relaxed);
		error = "The credential secret is empty or exceeds the Windows Credential Manager limit.";
		return false;
	}

	auto userName = Utf8ToWide(accountId);
	if (!userName) {
		request->status.store(Status::InvalidInput, memory_order_relaxed);
		error = "The account id is not valid UTF-8.";
		return false;
	}

	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = target->data();
	credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secret.data()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = userName->data();

	if (!CredWriteW(&credential, 0)) {
		request->status.store(Status::Failed, memory_order_relaxed);
		error = WindowsError("CredWriteW", GetLastError());
		return false;
	}

	error.clear();
	request->status.store(Status::Success, memory_order_relaxed);
	return true;
}

optional<string> SecureTokenStore::Load(const string &platformId, const string &accountId, string &error)
{
	auto request = RequestForCurrentThread();
	if (!RequestMayStart(request, error))
		return nullopt;
	auto target = CredentialTarget(platformId, accountId, error);
	if (!target) {
		request->status.store(Status::InvalidInput, memory_order_relaxed);
		return nullopt;
	}

	PCREDENTIALW credential = nullptr;
	if (!CredReadW(target->c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
		const DWORD code = GetLastError();
		if (code == ERROR_NOT_FOUND) {
			request->status.store(Status::NotFound, memory_order_relaxed);
			error.clear();
			return nullopt;
		}
		error = WindowsError("CredReadW", code);
		request->status.store(Status::Failed, memory_order_relaxed);
		return nullopt;
	}

	string secret(reinterpret_cast<const char *>(credential->CredentialBlob), credential->CredentialBlobSize);
	CredFree(credential);
	error.clear();
	request->status.store(Status::Success, memory_order_relaxed);
	return secret;
}

bool SecureTokenStore::Remove(const string &platformId, const string &accountId, string &error)
{
	auto request = RequestForCurrentThread();
	if (!RequestMayStart(request, error))
		return false;
	auto target = CredentialTarget(platformId, accountId, error);
	if (!target) {
		request->status.store(Status::InvalidInput, memory_order_relaxed);
		return false;
	}

	if (!CredDeleteW(target->c_str(), CRED_TYPE_GENERIC, 0)) {
		const DWORD code = GetLastError();
		if (code != ERROR_NOT_FOUND) {
			request->status.store(Status::Failed, memory_order_relaxed);
			error = WindowsError("CredDeleteW", code);
			return false;
		}
	}

	error.clear();
	request->status.store(Status::Success, memory_order_relaxed);
	return true;
}
