/*
 * Copyright 2026 Record Replay Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkRecordReplayDraw_DEFINED
#define SkRecordReplayDraw_DEFINED

#include "include/core/SkTypes.h"

// Replay draws a compositor frame in-process, on the compositor thread and with
// recorded events disallowed, only when the recorder asks for its pixels
// (components/viz/service/display/record_replay_render.cc in Chromium). Caches
// that recorded threads share are kept apart from such draws, so drawing or not
// never changes what recorded threads find in them.
SK_API void SkRecordReplayBeginIsolatedDraw();
SK_API void SkRecordReplayEndIsolatedDraw();

#endif
