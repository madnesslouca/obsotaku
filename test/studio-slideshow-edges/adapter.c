/* SPDX-License-Identifier: GPL-2.0-or-later
 * Compile into a private fixture core only. Never install these QA exports.
 */
#include "obs-internal.h"
EXPORT long studio_graph_show(obs_source_t *s) { return os_atomic_load_long(&s->show_refs); }
EXPORT long studio_graph_active(obs_source_t *s) { return os_atomic_load_long(&s->activate_refs); }
EXPORT void studio_graph_tick(obs_source_t *s) { obs_source_video_tick(s,0.016f); }
EXPORT void studio_graph_drain(void)
{ os_task_queue_wait(obs->destruction_task_thread); os_task_queue_wait(obs->destruction_task_thread); }
