/* Preserve the entire original reconnect/media scenario. Only the settings
 * constructor requests DBR, then inspect the real RTMP outputs and production
 * policy snapshots immediately after Start. No artificial estimator feedback;
 * this validates real RTMP/media, not throttled-network congestion. */
#include <utility/MultiStreamManager.hpp>
namespace { void verifyDbrPolicy(); }
struct DbrRequestedSettings : MultiStreamOutputSettings {
	DbrRequestedSettings() { dynamicBitrate = true; }
	~DbrRequestedSettings() noexcept(false) { verifyDbrPolicy(); }
};
#define MultiStreamOutputSettings DbrRequestedSettings
#include "../multistream-reconnect-check.cpp"
#undef MultiStreamOutputSettings
namespace {
void verifyDbrPolicy()
{
	if (!manager) return;
	for (const auto &entry : manager->ChannelOutputs()) {
		OBSDataAutoRelease settings = obs_output_get_settings(entry.output);
		check(!obs_data_get_bool(settings, "dyn_bitrate"), "real RTMP output disables independent shared-encoder DBR");
		check(obs_output_get_video_encoder(entry.output) == video, "real RTMP output preserves shared encoder identity");
	}
	for (const auto &entry : manager->Snapshot()) {
		check(entry.dynamicBitrateRequested && !entry.dynamicBitrateEffective &&
		      entry.dynamicBitrateReason == MultiStreamDynamicBitrateReason::SharedEncoderAuthority,
		      "real RTMP snapshot reports preserved intent and effective shared-encoder protection");
	}
	OBSDataAutoRelease settings = obs_encoder_get_settings(video);
	check(obs_data_get_int(settings, "bitrate") == 500, "real RTMP start preserves configured shared encoder bitrate");
}
}
