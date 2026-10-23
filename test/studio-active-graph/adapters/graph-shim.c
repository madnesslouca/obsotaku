#include "obs-internal.h"
EXPORT long studio_graph_show(obs_source_t *source) { return os_atomic_load_long(&source->show_refs); }
EXPORT long studio_graph_active(obs_source_t *source) { return os_atomic_load_long(&source->activate_refs); }
EXPORT void studio_graph_tick(obs_source_t *source) { obs_source_video_tick(source, 0.016f); }
/* Public obs_wait_for_destroy_queue refuses hosts without video+audio threads.
 * This private graphics-free host drains only its actual destruction queue. */
EXPORT void studio_graph_drain(void) {
 os_task_queue_wait(obs->destruction_task_thread);
 os_task_queue_wait(obs->destruction_task_thread);
}
EXPORT bool studio_graph_transfer_pair(obs_source_t *old1, obs_source_t *new1, obs_source_t *child1,
                                      obs_source_t *old2, obs_source_t *new2, obs_source_t *child2) {
 struct obs_active_child_move moves[2] = {{old1,new1,child1,NULL},{old2,new2,child2,NULL}};
 return obs_source_reparent_active_children(moves, 2);
}
