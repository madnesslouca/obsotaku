/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

/* Placeholder for platforms without a credential-store backend yet. It keeps
 * the frontend linkable and refuses every operation instead of silently
 * persisting OAuth tokens somewhere insecure. Replace with libsecret on Linux
 * and Keychain Services on macOS before shipping connected accounts there. */

#include "SecureTokenStore.hpp"

using namespace std;

static const char *UNSUPPORTED =
	"Connected accounts need an operating-system credential store, which is not implemented on this platform yet.";

bool SecureTokenStore::Save(const string &, const string &, const string &, string &error)
{
	error = UNSUPPORTED;
	return false;
}

optional<string> SecureTokenStore::Load(const string &, const string &, string &error)
{
	error = UNSUPPORTED;
	return nullopt;
}

bool SecureTokenStore::Remove(const string &, const string &, string &error)
{
	error = UNSUPPORTED;
	return false;
}
