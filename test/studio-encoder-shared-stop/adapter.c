/* SPDX-License-Identifier: GPL-2.0-or-later
 * Compile inside a private libobs fixture only; never install this adapter.
 */
#include "obs-internal.h"
EXPORT bool studio_test_encoder_initialize(obs_encoder_t *e)
{ return obs_encoder_initialize(e); }
EXPORT void studio_test_encoder_start(obs_encoder_t *e, encoded_callback_t cb, void *p)
{ obs_encoder_start(e, cb, p); }
EXPORT void studio_test_encoder_stop(obs_encoder_t *e, encoded_callback_t cb, void *p)
{ obs_encoder_stop(e, cb, p); }
