// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <obs.h>
#include <string>
class VST3Plugin;
std::string StudioVst3ParameterKey(const VST3Plugin *plugin, unsigned int id);
void ApplyStudioVst3Parameters(VST3Plugin *plugin, obs_data_t *settings);
void SaveStudioVst3Parameters(VST3Plugin *plugin, obs_data_t *settings);
void AddStudioVst3Parameters(VST3Plugin *plugin, obs_properties_t *properties);
