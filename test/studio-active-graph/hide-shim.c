/* Private fixture adapter only. No production algorithm modification. */
#include "obs-internal.h"
#include "obs-scene.h"
void hide_scene_audio_lock(obs_scene_t *scene) { pthread_mutex_lock(&scene->audio_mutex); }
void hide_scene_audio_unlock(obs_scene_t *scene) { pthread_mutex_unlock(&scene->audio_mutex); }

#include <errno.h>
size_t hide_item_pending(obs_sceneitem_t *item, long *active_refs)
{
 pthread_mutex_lock(&item->actions_mutex);
 size_t count = item->audio_actions.num;
 *active_refs = os_atomic_load_long(&item->active_refs);
 pthread_mutex_unlock(&item->actions_mutex);
 return count;
}
int hide_group_video_probe(obs_scene_t *scene)
{
 int result = pthread_mutex_trylock(&scene->video_mutex);
 if (result == 0) pthread_mutex_unlock(&scene->video_mutex);
 return result;
}
