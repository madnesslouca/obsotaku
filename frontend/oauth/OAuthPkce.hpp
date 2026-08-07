/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <string>

struct OAuthPkceData {
	std::string verifier;
	std::string challenge;
	std::string state;
};

class OAuthPkce {
public:
	static OAuthPkceData Generate();
	static std::string ChallengeForVerifier(const std::string &verifier);
};
