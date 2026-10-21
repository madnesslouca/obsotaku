#pragma once

#include "StudioVodRouting.hpp"
#include <QString>
#include <obs.h>

StudioVodPlan CurrentStudioVodRouting();
QString StudioVodRoutingHint(const StudioVodPlan &plan);
bool SetStudioSourceVodExcluded(obs_source_t *source, bool excluded);
