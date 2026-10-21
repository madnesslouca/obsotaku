#pragma once

#include <obs.h>
#include <string>
#include <vector>

class MultiStreamManager;

/* Apply the promoted channel's selected mix after native VOD setup and before
 * starting the main output. An ordinary profile service is left untouched. */
bool PrepareMultistreamPrimaryAudio(const MultiStreamManager &manager, obs_output_t *output,
				   const std::vector<obs_encoder_t *> &tracks, std::string &error);
