/* A retired registry snapshot is the last source owner at shutdown.
 * Its destroy callback must run before source-type resources are released. */
#include <obs.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
extern "C" void *studio_roots_attach(obs_canvas_t *, bool);
extern "C" void studio_roots_detach(void *);
extern "C" void *studio_roots_hold_snapshot(void);
extern "C" void studio_roots_drop_snapshot(void *);
namespace {
std::atomic<bool> typeClosed{false}, destroyedAfterType{false};
std::atomic<int> destroyed{0}, typesFreed{0};
int typeToken;
const char *name(void *) { return "Retired snapshot shutdown probe"; }
void *create(obs_data_t *, obs_source_t *s) { return s; }
void destroy(void *) { destroyedAfterType = typeClosed.load(); ++destroyed; }
void closeType(void *) { typeClosed = true; ++typesFreed; }
}
int main(int argc, char **argv)
{
	if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
	if (!obs_startup("en-US", nullptr, nullptr)) return 3;
	obs_audio_info ai{}; ai.samples_per_sec = 48000; ai.speakers = SPEAKERS_MONO;
	if (!obs_reset_audio(&ai)) return 4;
	obs_source_info info{};
	info.id = "studio_roots_shutdown_probe"; info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO; info.get_name = name; info.create = create;
	info.destroy = destroy; info.type_data = &typeToken; info.free_type_data = closeType;
	obs_register_source(&info);
	auto *source = obs_source_create_private(info.id, "QA retained root", nullptr);
	auto *canvas = obs_canvas_create_private("QA shutdown", nullptr, PROGRAM);
	obs_canvas_set_channel(canvas, 0, source);
	void *route = studio_roots_attach(canvas, true);
	void *reader = studio_roots_hold_snapshot();
	obs_canvas_set_channel(canvas, 0, nullptr);
	studio_roots_detach(route);
	obs_canvas_release(canvas);
	obs_source_release(source);
	studio_roots_drop_snapshot(reader); // No later writer/collector before shutdown.
	obs_wait_for_destroy_queue();
	const bool retained = destroyed.load() == 0;
	obs_shutdown();
	const bool passed = retained && destroyed == 1 && typesFreed == 1 && !destroyedAfterType;
	auto *result = obs_data_create();
	obs_data_set_bool(result, "passed", passed);
	obs_data_set_bool(result, "snapshot_was_last_source_owner", retained);
	obs_data_set_int(result, "destroy_calls", destroyed);
	obs_data_set_int(result, "type_free_calls", typesFreed);
	obs_data_set_bool(result, "source_destroyed_after_type_resources", destroyedAfterType);
	obs_data_set_string(result, "scope", "Synthetic source lifetime at native core shutdown; private registry reader adapter. No GPU, plugin unload or device validation.");
	std::ofstream(argv[1]) << obs_data_get_json(result) << '\n';
	obs_data_release(result);
	std::cout << (passed ? "PASS" : "FAIL") << " retired source destroyed before type resources\n";
	return passed ? 0 : 1;
}
