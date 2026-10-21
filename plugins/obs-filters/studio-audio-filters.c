#include <obs-module.h>
#include <math.h>

struct studio_audio_data {
	obs_source_t *context;
	float gain, pan;
	size_t channels;
};

static float bounded(double value, float low, float high, float fallback)
{
	return isfinite(value) ? fminf(high, fmaxf(low, (float)value)) : fallback;
}

static void studio_audio_update(void *data, obs_data_t *settings)
{
	struct studio_audio_data *state = data;
	state->gain = bounded(obs_data_get_double(settings, "gain"), 0.0f, 4.0f, 1.0f);
	state->pan = bounded(obs_data_get_double(settings, "pan"), -1.0f, 1.0f, 0.0f);
	state->channels = audio_output_get_channels(obs_get_audio());
}

static void *studio_audio_create(obs_data_t *settings, obs_source_t *context)
{
	struct studio_audio_data *state = bzalloc(sizeof(*state));
	state->context = context;
	studio_audio_update(state, settings);
	return state;
}

static void studio_audio_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "gain", 1.0);
	obs_data_set_default_double(settings, "pan", 0.0);
}

static const char *studio_gain_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Studio.Gain");
}

static const char *studio_pan_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Studio.StereoPanner");
}

static obs_properties_t *studio_gain_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, "gain", obs_module_text("Studio.Gain"), 0.0, 4.0, 0.01);
	return props;
}

static obs_properties_t *studio_pan_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_float_slider(props, "pan", obs_module_text("Studio.Pan"), -1.0, 1.0, 0.01);
	obs_property_set_enabled(p, audio_output_get_channels(obs_get_audio()) >= 2);
	return props;
}

static struct obs_audio_data *studio_gain_audio(void *data, struct obs_audio_data *audio)
{
	const struct studio_audio_data *state = data;
	for (size_t c = 0; c < state->channels; ++c) {
		float *samples = (float *)audio->data[c];
		if (samples) for (uint32_t i = 0; i < audio->frames; ++i) samples[i] *= state->gain;
	}
	return audio;
}

static struct obs_audio_data *studio_pan_audio(void *data, struct obs_audio_data *audio)
{
	const struct studio_audio_data *state = data;
	if (state->channels < 2 || !audio->data[0] || !audio->data[1]) return audio;
	float *left = (float *)audio->data[0], *right = (float *)audio->data[1];
	const float pan = state->pan;
	const bool mono = obs_source_get_speaker_layout(obs_filter_get_parent(state->context)) == SPEAKERS_MONO;
	// Equal-power mono panning and stereo crossfeed follow the Web Audio
	// StereoPanner algorithm. Other surround channels pass through untouched.
	const float angle = (mono ? (pan + 1.0f) * 0.5f : pan <= 0 ? pan + 1.0f : pan) * 1.57079632679f;
	const float l_gain = cosf(angle), r_gain = sinf(angle);
	for (uint32_t i = 0; i < audio->frames; ++i) {
		const float l = left[i], r = right[i];
		if (mono) {
			// libobs has already upmixed mono into the stereo output planes.
			const float sample = (l + r) * 0.70710678118f;
			left[i] = sample * l_gain; right[i] = sample * r_gain;
		} else if (pan <= 0) {
			left[i] = l + r * l_gain; right[i] = r * r_gain;
		} else {
			left[i] = l * l_gain; right[i] = r + l * r_gain;
		}
	}
	return audio;
}

struct obs_source_info studio_gain_filter = {
	.id = "studio_gain_filter", .type = OBS_SOURCE_TYPE_FILTER, .output_flags = OBS_SOURCE_AUDIO,
	.get_name = studio_gain_name, .create = studio_audio_create, .destroy = bfree,
	.update = studio_audio_update, .filter_audio = studio_gain_audio,
	.get_defaults = studio_audio_defaults, .get_properties = studio_gain_properties,
};
struct obs_source_info studio_pan_filter = {
	.id = "studio_pan_filter", .type = OBS_SOURCE_TYPE_FILTER, .output_flags = OBS_SOURCE_AUDIO,
	.get_name = studio_pan_name, .create = studio_audio_create, .destroy = bfree,
	.update = studio_audio_update, .filter_audio = studio_pan_audio,
	.get_defaults = studio_audio_defaults, .get_properties = studio_pan_properties,
};
