/* Uses production manager/libobs lifecycle with inert transport and simulated
 * DBR feedback. A failed connection reports congestion by changing the same
 * borrowed encoder settings, as the real RTMP controller does. No packets,
 * network, DSP, physical devices, config discovery or authenticated services. */
#include <obs.h>
#include <obs.hpp>
#include <utility/MultiStreamManager.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <iostream>
#include <vector>

namespace {
QJsonArray checks;
int feedbackControllers = 0;
int starts = 0;
int persistenceWrites = 0;
const char *name(void *) { return "inert DBR fixture"; }
const char *protocol(void *) { return "RTMP"; }
void destroy(void *) {}
void *encoderCreate(obs_data_t *, obs_encoder_t *encoder) { return encoder; }
bool encode(void *, encoder_frame *, encoder_packet *, bool *received) { *received = false; return true; }
size_t frames(void *) { return 1024; }
void *serviceCreate(obs_data_t *, obs_service_t *service) { return service; }
void *outputCreate(obs_data_t *, obs_output_t *output) { return output; }
bool outputStart(void *data)
{
	++starts;
	auto output = static_cast<obs_output_t *>(data);
	OBSDataAutoRelease outputSettings = obs_output_get_settings(output);
	if (obs_data_get_bool(outputSettings, "dyn_bitrate")) {
		++feedbackControllers;
		/* Simulates RTMP dbr_set_bitrate's public encoder mutation. It does
		 * not claim to reproduce its bandwidth estimator or socket timing. */
		auto encoder = obs_output_get_video_encoder(output);
		OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
		obs_data_set_int(settings, "bitrate", 400);
		obs_encoder_update(encoder, settings);
	}
	return true; /* Pending connection, no synthetic Live without capture. */
}
void outputStop(void *data, uint64_t) { obs_output_signal_stop(static_cast<obs_output_t *>(data), OBS_OUTPUT_SUCCESS); }
void packet(void *, encoder_packet *) {}
void check(bool pass, const std::string &label)
{
	checks.append(QJsonObject{{"check", QString::fromStdString(label)}, {"passed", pass}});
}
void drain()
{
	QEventLoop loop;
	QTimer::singleShot(100, &loop, &QEventLoop::quit);
	loop.exec();
}
std::vector<MultiStreamChannel> makeChannels(int count, bool disabledSecond = false)
{
	std::vector<MultiStreamChannel> result;
	for (int i = 0; i < count; ++i) {
		MultiStreamChannel channel;
		channel.id = "dbr-" + std::to_string(i);
		channel.displayName = channel.id;
		channel.server = "rtmp://127.0.0.1:1/live";
		channel.streamKey = "fixture";
		channel.enabled = !(disabledSecond && i == 1);
		result.push_back(channel);
	}
	return result;
}
void scenario(int count, bool externalConsumers, bool requested, bool disabledSecond)
{
	const std::string prefix = std::to_string(count) + "dest/external=" + std::to_string(externalConsumers) +
		"/requested=" + std::to_string(requested) + "/disabled=" + std::to_string(disabledSecond) + ": ";
	OBSDataAutoRelease initial = obs_data_create();
	obs_data_set_int(initial, "bitrate", 2500);
	OBSEncoderAutoRelease video = obs_video_encoder_create("dbr-video", "shared-video", initial, nullptr);
	OBSEncoderAutoRelease audio = obs_audio_encoder_create("dbr-audio", "shared-audio", nullptr, 0, nullptr);
	std::vector<OBSOutputAutoRelease> external;
	if (externalConsumers) {
		for (const char *consumer : {"primary", "recording", "replay"}) {
			OBSOutputAutoRelease out = obs_output_create("rtmp_output", consumer, nullptr, nullptr);
			obs_output_set_video_encoder(out, video);
			external.emplace_back(std::move(out));
		}
	}
	const int previousControllers = feedbackControllers;
	MultiStreamManager manager;
	std::string error;
	MultiStreamOutputSettings options;
	options.dynamicBitrate = requested;
	check(manager.Configure(makeChannels(count, disabledSecond), error) && manager.Start(video, audio, options, error), prefix + "start accepted");
	auto outputs = manager.ChannelOutputs();
	bool safeSettings = true;
	for (const auto &out : outputs) {
		OBSDataAutoRelease settings = obs_output_get_settings(out.output);
		safeSettings &= !obs_data_get_bool(settings, "dyn_bitrate");
	}
	check(safeSettings, prefix + "all additional outputs have effective DBR disabled");
	check(feedbackControllers == previousControllers, prefix + "no additional feedback controller mutates shared encoder");
	OBSDataAutoRelease effective = obs_encoder_get_settings(video);
	check(obs_data_get_int(effective, "bitrate") == 2500, prefix + "borrowed encoder bitrate remains 2500");
	bool identities = true;
	for (const auto &out : outputs) identities &= obs_output_get_video_encoder(out.output) == video;
	for (const auto &out : external) identities &= obs_output_get_video_encoder(out) == video;
	check(identities, prefix + "destinations and external consumers retain same encoder identity");
	check(options.dynamicBitrate == requested, prefix + "caller preference preserved");
#ifdef STUDIO_DBR_SNAPSHOT
	bool snapshots = true;
	for (const auto &snapshot : manager.Snapshot()) {
		snapshots &= snapshot.dynamicBitrateRequested == requested && !snapshot.dynamicBitrateEffective &&
			snapshot.dynamicBitrateReason == (requested ? MultiStreamDynamicBitrateReason::SharedEncoderAuthority : MultiStreamDynamicBitrateReason::NotRequested);
	}
	check(snapshots, prefix + "snapshot distinguishes request, effective policy and reason");
#endif
	if (disabledSecond) {
		check(manager.SetChannelEnabled("dbr-1", true, error), prefix + "disabled output enabled within session");
		check(manager.RestartChannel("dbr-1", error), prefix + "restart accepted");
		drain();
		check(feedbackControllers == previousControllers, prefix + "enable/restart keeps feedback disabled");
	}
	manager.Stop(true);
	drain();
	check(!manager.IsActive(), prefix + "stop settles inactive");
}
}
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	++persistenceWrites; error.clear(); return true;
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (argc != 2 || !obs_startup("en-US", nullptr, nullptr)) return 2;
	obs_encoder_info video{};
	video.id = "dbr-video"; video.type = OBS_ENCODER_VIDEO; video.codec = "h264";
	video.get_name = name; video.create = encoderCreate; video.destroy = destroy; video.encode = encode;
	obs_register_encoder(&video);
	auto audio = video;
	audio.id = "dbr-audio"; audio.type = OBS_ENCODER_AUDIO; audio.codec = "aac"; audio.get_frame_size = frames;
	obs_register_encoder(&audio);
	obs_service_info svc{};
	svc.id = "rtmp_custom"; svc.get_name = name; svc.create = serviceCreate; svc.destroy = destroy; svc.get_protocol = protocol;
	obs_register_service(&svc);
	obs_output_info output{};
	output.id = "rtmp_output"; output.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	output.get_name = name; output.create = outputCreate; output.destroy = destroy;
	output.start = outputStart; output.stop = outputStop; output.encoded_packet = packet;
	obs_register_output(&output);
	scenario(1, false, true, false);
	scenario(1, true, true, false);
	scenario(2, true, true, false);
	scenario(2, true, false, false);
	scenario(2, true, true, true);
	check(persistenceWrites == 1, "only explicit channel enable writes preference; no DBR preference writes");
	obs_shutdown();
	bool passed = true;
	for (const auto &c : checks) passed &= c.toObject()["passed"].toBool();
#ifdef _WIN32
	constexpr bool nativeWindows = true;
#else
	constexpr bool nativeWindows = false;
#endif
	QFile report(QString::fromLocal8Bit(argv[1]));
	if (!report.open(QIODevice::WriteOnly)) return 3;
	report.write(QJsonDocument(QJsonObject{{"passed", passed}, {"checks", checks}, {"starts", starts}, {"simulated_feedback_controllers", feedbackControllers},
		{"scope", "production manager/libobs lifecycle with inert transport and simulated DBR encoder feedback; no real network congestion or encoded media"}, {"native_windows_validated", nativeWindows}}).toJson());
	std::cout << "checks=" << checks.size() << " passed=" << passed << '\n';
	return passed ? 0 : 1;
}
