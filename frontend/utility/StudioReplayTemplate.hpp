#pragma once
#include "StudioOutputPreset.hpp"
struct BasicOutputHandler;

/* Selects the real live destination for this canvas, preferring the primary.
 * Different live audio selections without a primary are ambiguous for follow.
 * Idle preparation resolves private profile data only, never SetupOutputs(). */
bool ResolveStudioReplayTemplate(BasicOutputHandler *handler, StudioCanvasTarget canvas, StudioOutputPreset &result,
				 QString &error);
