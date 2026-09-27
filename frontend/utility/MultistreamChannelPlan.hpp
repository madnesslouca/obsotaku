/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "MultiStreamManager.hpp"

#include <vector>

namespace MultistreamChannelPlan {
MultiStreamChannel FirstReady(const std::vector<MultiStreamChannel> &channels);
void NormalizePortraitFit(std::vector<MultiStreamChannel> &channels);
bool RequiresH264(StreamPlatform platform);
} // namespace MultistreamChannelPlan
