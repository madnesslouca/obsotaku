#include "StudioVodRouting.hpp"

namespace {
bool ValidTrack(int index) { return index >= 0 && index < 6; }
uint32_t TrackBit(int index, int fallback)
{
	return 1u << (ValidTrack(index) ? index : fallback);
}
}

StudioVodPlan ResolveStudioVodRouting(StudioVodDestination profile, const std::vector<StudioVodDestination> &destinations)
{
	StudioVodPlan plan;
	plan.invalid = !ValidTrack(profile.liveTrack) || !ValidTrack(profile.vodTrack);
	// Protect the ordinary profile as well as every configured destination.
	// An unresolved managed account may leave the ordinary service in charge.
	plan.liveMask = TrackBit(profile.liveTrack, 0);
	plan.vodMask = profile.vodEnabled ? TrackBit(profile.vodTrack, 1) : 0;
	plan.configured = profile.vodEnabled;
	for (const auto &destination : destinations) {
		plan.invalid |= !ValidTrack(destination.liveTrack) ||
			(destination.vodEnabled && !ValidTrack(destination.vodTrack));
		plan.liveMask |= TrackBit(destination.liveTrack, 0);
		if (destination.vodEnabled) {
			plan.vodMask |= TrackBit(destination.vodTrack, 1);
			plan.configured = true;
		}
	}
	// A disabled output can still have its clean mix prepared for later use.
	if (!plan.vodMask) plan.vodMask = TrackBit(profile.vodTrack, 1);
	plan.conflict = (plan.liveMask & plan.vodMask) != 0;
	return plan;
}

uint32_t MergeStudioVodRouting(uint32_t current, uint32_t target, uint32_t included, uint32_t protectedLive)
{
	const uint32_t editable = target & ~protectedLive & 0x3fu;
	return (current & ~editable) | (included & editable);
}
