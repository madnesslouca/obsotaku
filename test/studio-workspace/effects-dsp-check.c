/* Exercises the production filter callbacks on deterministic PCM, without
 * opening a device, recording, or modifying a user's scene collection. */
#include <obs-module.h>
#include <media-io/audio-math.h>
#include <math.h>
#include <stdio.h>

extern struct obs_source_info compressor_filter, studio_gain_filter, studio_pan_filter, studio_voice_filter;
const char *obs_module_text(const char *text) { return text; }
static int checks, failures;
static void check(bool passed, const char *name)
{
	++checks; if (!passed) ++failures;
	printf("%s %s\n", passed ? "PASS" : "FAIL", name);
}
static const char *input_name(void *unused) { UNUSED_PARAMETER(unused); return "DSP fixture"; }
static void *input_create(obs_data_t *settings, obs_source_t *source) { UNUSED_PARAMETER(settings); return source; }
static void input_destroy(void *unused) { UNUSED_PARAMETER(unused); }
static obs_source_t *parent;
static obs_source_t *make(struct obs_source_info *info, const char *name, obs_data_t *settings)
{
	obs_source_t *filter = obs_source_create(info->id, name, settings, NULL);
	if (!filter) { check(false, "create production filter"); return NULL; }
	obs_source_filter_add(parent, filter);
	return filter;
}
static void release(obs_source_t *filter)
{
	obs_source_filter_remove(parent, filter); obs_source_release(filter);
}
static double compressed(float level, float knee)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_double(settings, "ratio", 4); obs_data_set_double(settings, "threshold", -20);
	obs_data_set_int(settings, "attack_time", 1); obs_data_set_int(settings, "release_time", 1);
	obs_data_set_double(settings, "knee", knee);
	obs_source_t *filter = make(&compressor_filter, "Compressor PCM", settings);
	float l[480], r[480]; struct obs_audio_data audio = {0};
	audio.frames = 480; audio.data[0] = (uint8_t *)l; audio.data[1] = (uint8_t *)r;
	for (int n = 0; n < 100; ++n) {
		for (int i = 0; i < 480; ++i) l[i] = r[i] = level;
		compressor_filter.filter_audio(obs_obj_get_data(filter), &audio);
	}
	double gain = 20.0 * log10(fabs(l[479] / level));
	release(filter); obs_data_release(settings); return gain;
}
static uint32_t random_state = 17;
static float noise(void)
{
	random_state = random_state * 1664525u + 1013904223u;
	return (float)((int32_t)(random_state >> 8) - 8388608) / 8388608.0f;
}
static double voice(int level, bool agc, bool speech)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "level", level); obs_data_set_bool(settings, "enable_agc", agc);
	obs_source_t *filter = make(&studio_voice_filter, "Voice PCM", settings);
	float l[480], r[480]; struct obs_audio_data audio = {0};
	audio.frames = 480; audio.data[0] = (uint8_t *)l; audio.data[1] = (uint8_t *)r;
	double energy = 0; size_t samples = 0; random_state = 17;
	for (int n = 0; n < 1600; ++n) {
		for (int i = 0; i < 480; ++i) {
			const double t = (n * 480 + i) / 48000.0;
			float value = .02f * noise();
			if (speech) {
				value = .0001f * noise();
				const double envelope = fmax(0, sin(2 * 3.141592653589793 * 3.1 * t));
				for (int harmonic = 1; harmonic <= 18; ++harmonic)
					value += .008f * (float)(envelope / harmonic * sin(harmonic * (2 * 3.141592653589793 * 125 * t + .8 * sin(2 * 3.141592653589793 * 2 * t))));
			}
			l[i] = r[i] = value;
		}
		audio.timestamp = 1000000000ULL + n * 10000000ULL;
		struct obs_audio_data *out = studio_voice_filter.filter_audio(obs_obj_get_data(filter), &audio);
		if (!out) continue;
		float *pcm = (float *)out->data[0];
		for (uint32_t i = 0; i < out->frames; ++i) {
			if (!isfinite(pcm[i]) || fabsf(pcm[i]) > 1.001f) { check(false, "voice output finite and bounded"); break; }
			if (n >= 1200) { energy += pcm[i] * pcm[i]; ++samples; }
		}
	}
	double rms = sqrt(energy / (double)samples);
	release(filter); obs_data_release(settings); return rms;
}
int main(void)
{
	if (!obs_startup("en-US", NULL, NULL)) return 2;
	struct obs_audio_info audio_info = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&audio_info)) return 2;
	struct obs_source_info input = {.id = "studio_dsp_fixture", .type = OBS_SOURCE_TYPE_INPUT, .output_flags = OBS_SOURCE_AUDIO,
		.get_name = input_name, .create = input_create, .destroy = input_destroy};
	obs_register_source(&input); obs_register_source(&compressor_filter); obs_register_source(&studio_gain_filter);
	obs_register_source(&studio_pan_filter); obs_register_source(&studio_voice_filter);
	parent = obs_source_create(input.id, "DSP input", NULL, NULL);
	obs_data_t *settings = obs_data_create();
	obs_source_t *gain = make(&studio_gain_filter, "Gain PCM", settings);
	float l[2] = {.2f, -.2f}, r[2] = {.1f, -.1f};
	struct obs_audio_data audio = {.data = {(uint8_t *)l, (uint8_t *)r}, .frames = 2};
	obs_data_set_double(settings, "gain", 2); obs_source_update(gain, settings);
	studio_gain_filter.filter_audio(obs_obj_get_data(gain), &audio);
	check(fabs(l[0] - .4) < 1e-6 && fabs(r[0] - .2) < 1e-6, "200 percent gain doubles both PCM channels");
	obs_data_set_double(settings, "gain", 0); obs_source_update(gain, settings);
	studio_gain_filter.filter_audio(obs_obj_get_data(gain), &audio);
	check(l[0] == 0 && r[0] == 0, "zero percent gain produces silence"); release(gain);
	obs_source_t *pan = make(&studio_pan_filter, "Pan PCM", NULL);
	for (int p = -1; p <= 1; ++p) {
		l[0] = .2f; r[0] = .1f; obs_data_set_double(settings, "pan", p); obs_source_update(pan, settings);
		studio_pan_filter.filter_audio(obs_obj_get_data(pan), &audio);
		check(fabs(l[0] - (p < 0 ? .3 : p > 0 ? 0 : .2)) < 1e-6 && fabs(r[0] - (p < 0 ? 0 : p > 0 ? .3 : .1)) < 1e-6,
			p < 0 ? "hard left folds stereo to left" : p > 0 ? "hard right folds stereo to right" : "center preserves stereo channels");
	}
	release(pan);
	float mono[480] = {0};
	struct obs_source_audio mono_audio = {.data = {(uint8_t *)mono}, .frames = 480, .speakers = SPEAKERS_MONO,
		.format = AUDIO_FORMAT_FLOAT, .samples_per_sec = 48000, .timestamp = 1000000000ULL};
	obs_source_output_audio(parent, &mono_audio);
	check(obs_source_get_speaker_layout(parent) == SPEAKERS_MONO, "mono fixture exposes original source layout");
	pan = make(&studio_pan_filter, "Mono pan PCM", NULL);
	for (int p = -1; p <= 1; ++p) {
		l[0] = r[0] = .2f * .70710678118f;
		obs_data_set_double(settings, "pan", p); obs_source_update(pan, settings);
		studio_pan_filter.filter_audio(obs_obj_get_data(pan), &audio);
		const double expected_l = p < 0 ? .2 : p > 0 ? 0 : .2 * .70710678118;
		const double expected_r = p < 0 ? 0 : p > 0 ? .2 : .2 * .70710678118;
		check(fabs(l[0] - expected_l) < 1e-6 && fabs(r[0] - expected_r) < 1e-6,
			p < 0 ? "mono equal-power hard left" : p > 0 ? "mono equal-power hard right" : "mono center preserves upmixed level");
	}
	release(pan);
	const double hard = compressed(.1f, 0), soft = compressed(.1f, 12);
	printf("MEASURE knee_at_threshold_db hard=%.5f soft=%.5f\n", hard, soft);
	check(fabs(hard) < .001 && fabs(soft + 1.125) < .002, "soft knee changes real PCM at threshold by calculated amount");
	check(fabs(compressed(.4f, 0) - compressed(.4f, 12)) < .002, "hard and soft curves agree above knee");
	check(fabs(compressed(.01f, 12)) < .001, "below knee remains unity gain");
	double low = voice(0, false, false), moderate = voice(1, false, false), high = voice(2, false, false);
	printf("MEASURE stationary_noise_rms low=%.9f moderate=%.9f high=%.9f\n", low, moderate, high);
	check(low > moderate * 1.5 && moderate > high * 1.5, "voice levels progressively attenuate deterministic stationary noise");
	double fixed = voice(1, false, true), automatic = voice(1, true, true);
	printf("MEASURE synthetic_voice_rms fixed=%.9f agc=%.9f delta_db=%.3f\n", fixed, automatic, 20 * log10(automatic / fixed));
	check(automatic > fixed * 1.15, "AGC changes actual synthetic voiced PCM loudness");
	obs_data_release(settings); obs_source_release(parent); obs_shutdown();
	printf("RESULT checks=%d failures=%d\n", checks, failures); return failures ? 1 : 0;
}
