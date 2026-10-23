/* Deterministic callback-order regression. Uses real libobs objects and the
 * production manager, with deliberately inert output/service/encoder types.
 * This is not a media or network acceptance test. No graphics/audio devices,
 * user configuration, secrets, plugin discovery or external services. */
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
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int starts = 0;
QJsonArray checks;
const char *name(void *) { return "inert cancellation fixture"; }
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
	/* Deliberately remains pending. Live is only valid with active capture;
	 * manufacturing Live without capture would make Stop's fixture invalid. */
	(void)data;
	return true;
}
void outputStop(void *data, uint64_t) { obs_output_signal_stop(static_cast<obs_output_t *>(data), OBS_OUTPUT_SUCCESS); }
void packet(void *, encoder_packet *) {}
void check(bool pass, const char *label)
{
	checks.append(QJsonObject{{"check", label}, {"passed", pass}});
}
void drain()
{
	QEventLoop loop;
	QTimer::singleShot(100, &loop, &QEventLoop::quit);
	loop.exec();
}
void emitSignal(obs_output_t *output, const char *signal)
{
	calldata_t params{};
	calldata_set_ptr(&params, "output", output);
	signal_handler_signal(obs_output_get_signal_handler(output), signal, &params);
	calldata_free(&params);
}
std::vector<MultiStreamChannel> channels()
{
	MultiStreamChannel a;
	a.id = "cancel-a"; a.displayName = "inert A";
	a.server = "rtmp://127.0.0.1:1/live"; a.streamKey = "fixture";
	a.audioMixIndex = 0;
	a.enabled = true;
	a.videoLayout = MultiStreamVideoLayout::Main;
	a.vodTrackEnabled = false;
	MultiStreamChannel b = a;
	b.id = "cancel-b"; b.displayName = "inert B";
	return {a, b};
}
}
bool MultistreamChannelStore::SetEnabled(const std::string &, bool, std::string &error)
{
	error.clear(); return true;
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (argc != 2 || !obs_startup("en-US", nullptr, nullptr)) return 2;
	obs_encoder_info v{};
	v.id = "cancel-video"; v.type = OBS_ENCODER_VIDEO; v.codec = "h264";
	v.get_name = name; v.create = encoderCreate; v.destroy = destroy; v.encode = encode;
	obs_register_encoder(&v);
	obs_encoder_info a = v;
	a.id = "cancel-audio"; a.type = OBS_ENCODER_AUDIO; a.codec = "aac"; a.get_frame_size = frames;
	obs_register_encoder(&a);
	obs_service_info svc{};
	svc.id = "rtmp_custom"; svc.get_name = name; svc.create = serviceCreate; svc.destroy = destroy;
	svc.get_protocol = protocol;
	obs_register_service(&svc);
	obs_output_info out{};
	out.id = "rtmp_output"; out.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	out.get_name = name; out.create = outputCreate; out.destroy = destroy;
	out.start = outputStart; out.stop = outputStop; out.encoded_packet = packet;
	obs_register_output(&out);
	{
		OBSEncoderAutoRelease video = obs_video_encoder_create("cancel-video", "inert-video", nullptr, nullptr);
		OBSEncoderAutoRelease audio = obs_audio_encoder_create("cancel-audio", "inert-audio", nullptr, 0, nullptr);
		MultiStreamOutputSettings options;
		std::string error;
		{
			MultiStreamManager manager;
			check(manager.Configure(channels(), error), "configure two inert destinations");
			int previous = starts;
			bool stop = false;
			manager.SetStateCallback([&](const auto &s) {
				if (!stop && s.state == MultiStreamChannelState::Starting) { stop = true; manager.Stop(true); }
			});
			check(!manager.Start(video, audio, options, error), "Start reports cancellation from Starting callback");
			check(starts == previous, "Stop during Starting callback prevents both native starts");
			drain();
			check(!manager.IsActive(), "cancelled Start settles inactive");
			manager.SetStateCallback({}); manager.Stop(true); drain();
		}
		{
			MultiStreamManager manager;
			check(manager.Configure(channels(), error), "configure callback-disable scenario");
			int previous = starts;
			bool disabled = false;
			manager.SetStateCallback([&](const auto &s) {
				if (!disabled && s.id == "cancel-a" && s.state == MultiStreamChannelState::Starting) {
					disabled = true; check(manager.SetChannelEnabled("cancel-a", false, error), "disable accepted inside Starting callback");
				}
			});
			check(manager.Start(video, audio, options, error), "other destination starts when A cancels itself");
			check(starts == previous + 1 && !manager.ConfiguredChannels()[0].enabled,
			      "only B reaches native start while A remains disabled");
			manager.SetStateCallback({}); manager.Stop(true); drain();
		}
		{
			MultiStreamManager manager;
			check(manager.Configure(channels(), error), "configure worker-stop scenario");
			int previous = starts;
			bool stopped = false;
			manager.SetStateCallback([&](const auto &s) {
				if (!stopped && s.state == MultiStreamChannelState::Starting) {
					stopped = true; std::thread([&]() { manager.Stop(true); }).join();
				}
			});
			check(!manager.Start(video, audio, options, error) && starts == previous,
			      "worker cancellation invalidates start before owner-thread stop is dispatched");
			drain();
			check(!manager.IsActive(), "worker stop settles inactive after queued work drains");
			manager.SetStateCallback({}); manager.Stop(true); drain();
		}
		{
			MultiStreamManager manager;
			check(manager.Configure(channels(), error), "configure reentrant-new-session scenario");
			int previous = starts;
			bool replaced = false;
			manager.SetStateCallback([&](const auto &s) {
				if (!replaced && s.state == MultiStreamChannelState::Starting) {
					replaced = true; manager.Stop(true); drain();
					check(manager.Start(video, audio, options, error), "new session starts inside cancelled callback");
				}
			});
			check(!manager.Start(video, audio, options, error), "old Start reports cancellation after replacement");
			check(starts == previous + 2 && manager.IsActive(), "old Start cannot start either retired output or stop the new session");
			manager.SetStateCallback({}); manager.Stop(true); drain();
		}
		{
			MultiStreamManager manager;
			check(manager.Configure(channels(), error) && manager.Start(video, audio, options, error), "fresh two-destination session starts");
			int previous = starts;
			bool stop = false;
			manager.SetStateCallback([&](const auto &s) {
				if (!stop && s.id == "cancel-b" && s.state == MultiStreamChannelState::Starting) { stop = true; manager.Stop(true); }
			});
			check(manager.RestartChannel("cancel-b", error), "restart requested before callback cancellation");
			drain();
			check(stop && starts == previous, "Stop during deferred restart prevents native start");
			check(!manager.IsActive(), "cancelled restart settles inactive");
			manager.SetStateCallback({}); manager.Stop(true); drain();
		}
		{
			MultiStreamManager manager;
			auto config = channels(); config[1].enabled = false;
			check(manager.Configure(config, error) && manager.Start(video, audio, options, error), "session prepares disabled destination lazily");
			int previous = starts;
			bool stop = false;
			manager.SetStateCallback([&](const auto &s) {
				if (!stop && s.id == "cancel-b" && s.state == MultiStreamChannelState::Starting) { stop = true; manager.Stop(true); }
			});
			check(!manager.SetChannelEnabled("cancel-b", true, error), "enable reports cancellation from Starting callback");
			check(starts == previous, "Stop while enabling prevents native start");
			drain();
			check(!manager.IsActive(), "cancelled enable settles inactive");
			int callbacks = 0;
			manager.SetStateCallback([&](const auto &) { ++callbacks; });
			for (const auto &entry : manager.ChannelOutputs()) { emitSignal(entry.output, "start"); emitSignal(entry.output, "reconnect"); }
			check(callbacks == 0 && !manager.IsActive(), "late start and reconnect signals cannot resurrect stopped UI");
			manager.SetStateCallback({}); manager.Stop(true); drain();
		}
	}
	obs_shutdown();
	bool passed = true;
	for (const auto &c : checks) passed &= c.toObject()["passed"].toBool();
	QFile report(QString::fromLocal8Bit(argv[1]));
	if (!report.open(QIODevice::WriteOnly)) return 3;
	report.write(QJsonDocument(QJsonObject{{"passed", passed}, {"checks", checks}, {"scope", "production manager and real libobs lifecycle; inert transport, not media/network"}, {"platform", "Linux"}, {"native_windows_validated", false}}).toJson());
	std::cout << "checks=" << checks.size() << " passed=" << passed << '\n';
	return passed ? 0 : 1;
}
