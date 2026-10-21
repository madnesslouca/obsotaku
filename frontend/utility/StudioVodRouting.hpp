#pragma once

#include <cstdint>
#include <vector>

struct StudioVodDestination {
	int liveTrack = 0;
	int vodTrack = 1;
	bool vodEnabled = false;
};
struct StudioVodPlan {
	uint32_t liveMask = 1;
	uint32_t vodMask = 2;
	bool configured = false;
	bool conflict = false;
	bool invalid = false;
	bool activeSession = false;
	bool effective = false;
	bool unknownLayout = false;
};

StudioVodPlan ResolveStudioVodRouting(StudioVodDestination profile,
				      const std::vector<StudioVodDestination> &destinations);
uint32_t MergeStudioVodRouting(uint32_t current, uint32_t target, uint32_t included, uint32_t protectedLive);
