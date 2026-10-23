#include "obs-internal.h"
#include "obs-scene.h"
int preflight_scene_audio_probe(obs_scene_t *scene) {
 int result=pthread_mutex_trylock(&scene->audio_mutex);
 if(!result)pthread_mutex_unlock(&scene->audio_mutex);
 return result;
}
