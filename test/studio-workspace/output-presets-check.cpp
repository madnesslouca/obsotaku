#include <utility/StudioOutputPreset.hpp>
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void Check(bool passed, const char *name)
{
	++checks;
	if (!passed) {
		std::fprintf(stderr, "FAIL: %s\n", name);
		std::exit(1);
	}
}
} // namespace
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	StudioOutputPreset source;
	source.encoderId = "fixture_h264";
	source.origin = "stream-effective";
	source.width = 1920;
	source.height = 1080;
	source.fpsDivisor = 2;
	OBSDataAutoRelease video = obs_data_create();
	source.videoSettings = video.Get();
	obs_data_set_default_int(video, "bitrate", 6200);
	obs_data_set_string(video, "rate_control", "CBR");
	OBSDataAutoRelease nested = obs_data_create();
	obs_data_set_string(nested, "mode", "original");
	obs_data_set_obj(video, "nested", nested);
	OBSDataArrayAutoRelease layers = obs_data_array_create();
	obs_data_array_push_back(layers, nested);
	obs_data_set_array(video, "layers", layers);
	StudioAudioPreset audio;
	audio.encoderId = "fixture_aac";
	audio.mixerIndex = 4;
	OBSDataAutoRelease settings = obs_data_create();
	audio.settings = settings.Get();
	obs_data_set_default_int(settings, "bitrate", 160);
	obs_data_set_obj(settings, "nested", nested);
	source.audio.push_back(std::move(audio));
	StudioOutputPreset copy = source;
	Check(copy.videoSettings.Get() != source.videoSettings.Get(), "copy owns video data");
	Check(copy.audio[0].settings.Get() != source.audio[0].settings.Get(), "copy owns audio data");
	Check(obs_data_get_int(copy.videoSettings, "bitrate") == 6200, "effective video defaults frozen");
	Check(obs_data_get_int(copy.audio[0].settings, "bitrate") == 160, "effective audio defaults frozen");
	obs_data_set_int(video, "bitrate", 9900);
	obs_data_set_string(nested, "mode", "changed");
	Check(obs_data_get_int(copy.videoSettings, "bitrate") == 6200, "live mutation cannot change snapshot");
	OBSDataAutoRelease copyNested = obs_data_get_obj(copy.videoSettings, "nested");
	Check(QString::fromUtf8(obs_data_get_string(copyNested, "mode")) == "original",
	      "nested video settings independent");
	OBSDataArrayAutoRelease copyLayers = obs_data_get_array(copy.videoSettings, "layers");
	OBSDataAutoRelease layer = obs_data_array_item(copyLayers, 0);
	Check(QString::fromUtf8(obs_data_get_string(layer, "mode")) == "original", "nested arrays independent");
	OBSDataAutoRelease copyAudioNested = obs_data_get_obj(copy.audio[0].settings, "nested");
	Check(QString::fromUtf8(obs_data_get_string(copyAudioNested, "mode")) == "original",
	      "nested audio settings independent");
	StudioOutputPreset assigned;
	assigned = copy;
	obs_data_set_int(copy.videoSettings, "bitrate", 4500);
	obs_data_set_int(copy.audio[0].settings, "bitrate", 128);
	Check(obs_data_get_int(assigned.videoSettings, "bitrate") == 6200, "copy assignment independent video");
	Check(obs_data_get_int(assigned.audio[0].settings, "bitrate") == 160, "copy assignment independent audio");
	assigned = assigned;
	Check(obs_data_get_int(assigned.videoSettings, "bitrate") == 6200, "self assignment preserves preset");
	const auto summary = assigned.Summary();
	Check(summary["origin"] == "stream-effective", "summary origin");
	Check(summary["width"].toInt() == 1920 && summary["height"].toInt() == 1080, "summary dimensions");
	Check(summary["fps_divisor"].toInt() == 2, "summary FPS divisor");
	Check(summary["bitrate_kbps"].toInt() == 6200 && summary["rate_control"] == "CBR",
	      "summary rate control and bitrate");
	obs_data_set_string(assigned.videoSettings, "sensitive-fixture-key", "never-output");
	Check(!QJsonDocument(assigned.Summary()).toJson().contains("never-output"),
	      "summary excludes arbitrary encoder settings");
	assigned.canvas = StudioCanvasTarget::Portrait;
	assigned.origin = "custom";
	Check(assigned.Summary()["canvas"] == "portrait" && assigned.Summary()["origin"] == "custom",
	      "summary canvas and explicit custom origin");
	StudioOutputPreset empty, emptyCopy = empty;
	Check(!emptyCopy.videoSettings && emptyCopy.audio.empty(), "empty snapshot safe");
	std::printf("Studio output preset data: %d checks passed\n", checks);
}
