/* Compile the real Fade plugin TU. Only shader creation is substituted in its
 * registration; core transition objects/locks/edges/start/clear/swap are real.
 * No visual/video render or shader correctness is claimed by this fixture. */
#include "transition-fade.c"
extern struct obs_source_info cut_transition;
static void *fade_shader_sentinel_create(obs_data_t *settings, obs_source_t *source)
{
 struct fade_info *fade = bzalloc(sizeof(*fade));
 fade->source = source;
 UNUSED_PARAMETER(settings);
 return fade;
}
void studio_register_transition_probes(void)
{
 obs_register_source(&cut_transition);
 struct obs_source_info sentinel = fade_transition;
 sentinel.create = fade_shader_sentinel_create;
 obs_register_source(&sentinel);
}
