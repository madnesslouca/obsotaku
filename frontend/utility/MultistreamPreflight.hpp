/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "MultiStreamManager.hpp"

#include <string>
#include <vector>

struct OutputVideoSettings {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t framerate = 0;
	/* Zero when the active output mode does not expose a simple bitrate,
	 * which is the case for advanced mode with a custom encoder. */
	uint32_t videoBitrateKbps = 0;
	uint32_t audioBitrateKbps = 0;
};

enum class PreflightSeverity {
	/* The destination still accepts the stream but will transcode or letterbox. */
	Advisory,
	/* The platform rejects or badly degrades the stream at these settings. */
	Warning,
};

struct PreflightFinding {
	std::string channelId;
	std::string channelDisplayName;
	PreflightSeverity severity = PreflightSeverity::Advisory;
	/* Already localized and ready to show. */
	std::string message;
};

/* Compares the active output settings against what each enabled destination
 * publishes, so the mismatch shows up before going live instead of after. */
class MultistreamPreflight {
public:
	static OutputVideoSettings CurrentOutputSettings();
	static std::vector<PreflightFinding> Check(const std::vector<MultiStreamChannel> &channels,
						   const OutputVideoSettings &settings);
};
