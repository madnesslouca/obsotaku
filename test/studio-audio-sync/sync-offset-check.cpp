/* Real libobs audio regression. No app, graphics, encoder or hardware endpoint.
 * The private test core contributes only an audio-view attach/detach adapter. */
#include <obs.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <atomic>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
extern "C" void *studio_audio_sync_attach();
extern "C" void studio_audio_sync_detach(void *);
namespace {
std::mutex captureMutex;
bool capturing = false;
std::vector<float> samples;
std::atomic<bool> stop{false}, backlogFailure{false};
std::atomic<uint64_t> maxLate{0}, maxDispatch{0}, blocks{0}, catchups{0};
void maxValue(std::atomic<uint64_t> &value, uint64_t n)
{
	auto previous = value.load();
	while (n > previous && !value.compare_exchange_weak(previous, n)) {}
}
void capture(void *, size_t mix, audio_data *data)
{
	if (mix != 0 || !data->data[0]) return;
	std::lock_guard lock(captureMutex);
	if (capturing) {
		auto *p = reinterpret_cast<const float *>(data->data[0]);
		samples.insert(samples.end(), p, p + data->frames);
	}
}
const char *sourceName(void *) { return "Continuous-clock sync-offset QA"; }
void *create(obs_data_t *, obs_source_t *s) { return s; }
void destroy(void *) {}
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::string corePath()
{
#ifdef _WIN32
	HMODULE mod = GetModuleHandleA("obs.dll");
	char p[32768] = {};
	GetModuleFileNameA(mod, p, sizeof(p));
	return p;
#else
	Dl_info info{};
	// A direct imported function address may point to the executable's PLT.
	return dladdr(dlsym(RTLD_DEFAULT, "obs_startup"), &info) ? info.dli_fname : "unknown";
#endif
}
void produce(obs_source_t *music, obs_source_t *reference)
{
	const uint64_t anchor = os_gettime_ns();
	uint64_t index = 0;
	unsigned consecutive = 0;
	while (!stop.load()) {
		const uint64_t ts = anchor + index * 1000000000ULL / 48000;
		os_sleepto_ns_fast(ts);
		if (stop.load()) break;
		const uint64_t begin = os_gettime_ns(), late = begin > ts ? begin - ts : 0;
		maxValue(maxLate, late);
		if (late >= 20000000) { ++consecutive; catchups.fetch_add(1); } else consecutive = 0;
		if (late > 100000000 || consecutive > 3) { backlogFailure.store(true); break; }
		std::array<float,960> a{}, b{};
		for (size_t i = 0; i < a.size(); ++i) {
			const double t = double(index+i)/48000.;
			const bool on = ((index+i)%48000)<9600; //1s period avoids +/-250ms alias.
			a[i] = on*.040*std::sin(2*3.141592653589793*997*t);
			b[i] = on*.030*std::sin(2*3.141592653589793*2003*t);
		}
		obs_source_audio d{};
		d.frames = 960; d.samples_per_sec = 48000; d.speakers = SPEAKERS_MONO;
		d.format = AUDIO_FORMAT_FLOAT; d.timestamp = ts;
		d.data[0] = reinterpret_cast<uint8_t *>(a.data()); obs_source_output_audio(music,&d);
		d.data[0] = reinterpret_cast<uint8_t *>(b.data()); obs_source_output_audio(reference,&d);
		maxValue(maxDispatch,os_gettime_ns()-begin);
		index += 960; blocks.fetch_add(1);
	}
}
struct Stage { const char *name; int offset; };
const Stage stages[]={{"initial_plus100",100},{"zero_baseline",0},{"zero_to_plus250",250},
	{"plus250_to_zero",0},{"zero_to_plus100",100},{"plus100_to_minus100",-100},
	{"minus100_to_zero",0},{"zero_noop",0},{"zero_to_plus250_repeat",250},
	{"plus250_to_plus100",100},{"plus100_to_zero",0}};
}
int main(int argc,char **argv)
{
	if(argc==2 && std::string(argv[1])=="--core-path") { std::cout<<corePath()<<'\n';return 0; }
	if(argc!=2) { std::cerr<<"Usage: studio-audio-sync-check FRESH_PRIVATE_ROOT\n";return 2; }
	const auto root=std::filesystem::absolute(argv[1]);
	if(std::filesystem::exists(root/"capture.json")) { std::cerr<<"Refusing existing capture report\n";return 2; }
	std::filesystem::create_directories(root);
	obs_source_t *music=nullptr,*reference=nullptr; void *view=nullptr; bool started=false,connected=false;
	std::thread producer;
	obs_data_t *report=obs_data_create(); obs_data_array_t *entries=obs_data_array_create();
	std::string failure;
	try {
		require(obs_startup("en-US",nullptr,nullptr),"obs_startup failed"); started=true;
		obs_audio_info ai{}; ai.samples_per_sec=48000; ai.speakers=SPEAKERS_MONO;
		require(obs_reset_audio(&ai),"48k mono audio reset failed");
		view=studio_audio_sync_attach(); require(view,"test audio-view attach failed");
		obs_source_info info{}; info.id="studio_continuous_audio_sync_qa"; info.type=OBS_SOURCE_TYPE_INPUT;
		info.output_flags=OBS_SOURCE_AUDIO; info.get_name=sourceName; info.create=create; info.destroy=destroy;
		obs_register_source(&info);
		music=obs_source_create_private(info.id,"QA997 offset",nullptr);
		reference=obs_source_create_private(info.id,"QA2003 reference",nullptr);
		require(music&&reference,"private source creation failed");
		obs_source_set_audio_mixers(music,1); obs_source_set_audio_mixers(reference,1);
		obs_source_set_sync_offset(music,100000000);
		obs_set_output_source(1,music); obs_set_output_source(2,reference);
		audio_convert_info ac{};ac.samples_per_sec=48000;ac.format=AUDIO_FORMAT_FLOAT_PLANAR;ac.speakers=SPEAKERS_MONO;
		require(audio_output_connect(obs_get_audio(),0,&ac,capture,nullptr),"real audio_output_connect failed");connected=true;
		producer=std::thread(produce,music,reference);
		for(const auto &s:stages) {
			obs_source_set_sync_offset(music,int64_t(s.offset)*1000000);
			require(obs_source_get_sync_offset(music)==int64_t(s.offset)*1000000,"effective sync getter mismatch");
			std::this_thread::sleep_for(std::chrono::milliseconds(900));
			require(!backlogFailure.load(),"producer clock backlog during settling");
			{std::lock_guard lock(captureMutex);samples.clear();capturing=true;}
			std::this_thread::sleep_for(std::chrono::milliseconds(2600));
			std::vector<float> data;
			{std::lock_guard lock(captureMutex);capturing=false;data=std::move(samples);}
			require(!backlogFailure.load(),"producer clock backlog during capture");
			require(data.size()>=96000,"real mixed PCM shorter than2s");
			const std::string filename=std::string(s.name)+".f32";
			std::ofstream f(root/filename,std::ios::binary);f.write(reinterpret_cast<const char *>(data.data()),data.size()*sizeof(float));
			require(bool(f),"private PCM write failed");
			obs_data_t *entry=obs_data_create();obs_data_set_string(entry,"name",s.name);obs_data_set_string(entry,"file",filename.c_str());
			obs_data_set_int(entry,"offset_ms",s.offset);obs_data_set_int(entry,"frames",data.size());obs_data_array_push_back(entries,entry);obs_data_release(entry);
			std::cout<<s.name<<" offset="<<s.offset<<" frames="<<data.size()<<std::endl;
		}
	} catch(const std::exception &e) {failure=e.what();}
	stop.store(true);if(producer.joinable()) producer.join();
	if(connected) audio_output_disconnect(obs_get_audio(),0,capture,nullptr);
	if(started) {obs_set_output_source(1,nullptr);obs_set_output_source(2,nullptr);}
	if(music) {obs_source_remove(music);obs_source_release(music);}
	if(reference) {obs_source_remove(reference);obs_source_release(reference);}
	if(view) studio_audio_sync_detach(view);
	obs_data_set_bool(report,"capture_passed",failure.empty());obs_data_set_string(report,"error",failure.c_str());
	obs_data_set_string(report,"loaded_core",corePath().c_str());obs_data_set_string(report,"scope","real libobs source/mixer/audio thread; test-only audio view adapter; no GPU, Qt app, encoder or device");
	obs_data_set_int(report,"sample_rate",48000);obs_data_set_int(report,"pulse_period_ms",1000);obs_data_set_int(report,"pulse_on_ms",200);
	obs_data_set_int(report,"program_delay_tolerance_ms",30);obs_data_set_int(report,"minimum_pcm_frames",96000);
	obs_data_set_double(report,"max_lateness_ms",double(maxLate.load())/1000000.);obs_data_set_double(report,"max_dispatch_ms",double(maxDispatch.load())/1000000.);
	obs_data_set_int(report,"blocks",blocks.load());obs_data_set_int(report,"catchups",catchups.load());obs_data_set_bool(report,"backlog_failure",backlogFailure.load());
	obs_data_set_array(report,"stages",entries);
	std::ofstream json(root/"capture.json");json<<obs_data_get_json_pretty(report)<<'\n';json.close();
	obs_data_array_release(entries);obs_data_release(report);
	if(started) {obs_wait_for_destroy_queue();obs_shutdown();}
	if(!failure.empty()) std::cerr<<failure<<'\n';return failure.empty()?0:1;
}
