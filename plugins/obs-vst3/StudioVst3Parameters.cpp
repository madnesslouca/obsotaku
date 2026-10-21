// SPDX-License-Identifier: GPL-3.0-or-later
#include "StudioVst3Parameters.hpp"
#include "VST3Plugin.h"
#include "obs-vst3.h"
#include <public.sdk/source/vst/utility/stringconvert.h>
#include <algorithm>
#include <cmath>
using namespace Steinberg;
using namespace Steinberg::Vst;
std::string StudioVst3ParameterKey(const VST3Plugin *plugin,unsigned int id)
{
    return "vst3_param_"+plugin->filterData->vst3Id+"_"+std::to_string(id);
}
template<class Fn> static void parameters(VST3Plugin *plugin,Fn fn)
{
    if(!plugin || !plugin->editController)return;
    for(int i=0;i<plugin->editController->getParameterCount();++i){
        ParameterInfo info{};
        if(plugin->editController->getParameterInfo(i,info)!=kResultOk ||
           (info.flags & (ParameterInfo::kIsReadOnly|ParameterInfo::kIsHidden)))continue;
        fn(info,StudioVst3ParameterKey(plugin,info.id));
    }
}
void ApplyStudioVst3Parameters(VST3Plugin *plugin,obs_data_t *settings)
{
    parameters(plugin,[&](const ParameterInfo &info,const std::string &key){
        if(!obs_data_has_user_value(settings,key.c_str())) {
            obs_data_set_double(settings,key.c_str(),plugin->editController->getParamNormalized(info.id));return;
        }
        double value=obs_data_get_double(settings,key.c_str());if(!std::isfinite(value))return;
        value=std::clamp(value,0.0,1.0);
        if(std::abs(value-plugin->editController->getParamNormalized(info.id))<1e-12)return;
        plugin->editController->setParamNormalized(info.id,value);
        plugin->guiToDsp.addChange(info.id,value,0);
    });
}
void SaveStudioVst3Parameters(VST3Plugin *plugin,obs_data_t *settings)
{
    parameters(plugin,[&](const ParameterInfo &info,const std::string &key){
        obs_data_set_double(settings,key.c_str(),plugin->editController->getParamNormalized(info.id));
    });
}
void AddStudioVst3Parameters(VST3Plugin *plugin,obs_properties_t *properties)
{
    parameters(plugin,[&](const ParameterInfo &info,const std::string &key){
        auto label=StringConvert::convert(info.title);
        auto units=StringConvert::convert(info.units);
        if(info.stepCount<=0 || info.stepCount>128) label+=" (0–1)";
        obs_property_t *property;
        if(info.stepCount==1)property=obs_properties_add_list(properties,key.c_str(),label.c_str(),OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_FLOAT);
        else if(info.stepCount>1 && info.stepCount<=128)property=obs_properties_add_list(properties,key.c_str(),label.c_str(),OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_FLOAT);
        else property=obs_properties_add_float_slider(properties,key.c_str(),label.c_str(),0.0,1.0,.001);
        if(info.stepCount>0 && info.stepCount<=128){
            for(int step=0;step<=info.stepCount;++step){double value=double(step)/info.stepCount;String128 text{};
                plugin->editController->getParamStringByValue(info.id,value,text);
                auto display=StringConvert::convert(text);if(display.empty())display=std::to_string(step);
                obs_property_list_add_float(property,display.c_str(),value);
            }
        }
        String128 text{};plugin->editController->getParamStringByValue(info.id,plugin->editController->getParamNormalized(info.id),text);
        auto hint=StringConvert::convert(text)+" "+units+"\nVST3 normalized parameter (0–1). Use the plug-in editor for its native display and advanced controls.";
        obs_property_set_long_description(property,hint.c_str());
    });
}
