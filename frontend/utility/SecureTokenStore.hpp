/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <optional>
#include <string>

class SecureTokenStore {
public:
	static bool Save(const std::string &platformId, const std::string &accountId, const std::string &secret,
			 std::string &error);
	static std::optional<std::string> Load(const std::string &platformId, const std::string &accountId,
					       std::string &error);
	static bool Remove(const std::string &platformId, const std::string &accountId, std::string &error);
};
