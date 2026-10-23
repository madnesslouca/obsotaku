#pragma once
#include <utility/StreamPlatform.hpp>
#include <string>
#include <vector>
struct MultiStreamChannel {StreamPlatform platform=StreamPlatform::CustomRtmp;std::string id,streamKey,accountId;};
class MultistreamChannelStore {public:static std::vector<MultiStreamChannel> Load();};
