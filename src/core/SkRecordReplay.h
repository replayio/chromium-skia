/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkRecordReplay_DEFINED
#define SkRecordReplay_DEFINED

#include <atomic>
#include <limits>

extern void SkRecordReplayPrint(const char* format, ...);
extern void SkRecordReplayWarning(const char* format, ...);
extern void SkRecordReplayAssert(const char* format, ...);
extern void SkRecordReplayDiagnostic(const char* format, ...);
extern void SkRecordReplayRegisterPointer(const void* ptr);
extern void SkRecordReplayUnregisterPointer(const void* ptr);
extern int SkRecordReplayPointerId(const void* ptr);

extern bool SkRecordReplayFeatureEnabled(const char* feature, const char* subfeature);
extern bool SkRecordReplayIsRecordingOrReplaying(const char* feature = nullptr,
                                                 const char* subfeature = nullptr);
extern bool SkRecordReplayAreEventsDisallowed(const char* why = nullptr);
// Whether the driver disallows recorded events on this thread, regardless of
// the disallow-events feature.
extern bool SkRecordReplayAreEventsDisallowedByDriver();
// Whether this thread is in a draw started by SkRecordReplayBeginIsolatedDraw().
extern bool SkRecordReplayInIsolatedDraw();

// Isolated draws (SkRecordReplayBeginIsolatedDraw) take IDs from a range of
// their own: an ID they took from a shared counter would be given again to an
// object a recorded thread creates later. Their IDs count down from the
// largest value, and shared counters count up, so they would only meet where a
// counter wraps. Never takes 0. Sets |id| and returns true in an isolated
// draw.
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
// Checks of what isolated draws touch (record_replay_isolated_draw_checks):
// prints |what| in an isolated draw, or, for the second, wherever recorded
// events are disallowed outside one.
extern void SkRecordReplayIsolatedDrawCheck(const char* what, const void* address);
extern void SkRecordReplayDisallowedIdCheck(const char* what, const void* address);
#endif

template <typename T>
inline bool SkRecordReplayTakeIsolatedId(T* id) {
  if (!SkRecordReplayInIsolatedDraw()) {
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    SkRecordReplayDisallowedIdCheck("Skia ID", __builtin_return_address(0));
#endif
    return false;
  }
  static std::atomic<T> next{std::numeric_limits<T>::max()};
  do {
    *id = next.fetch_sub(1, std::memory_order_relaxed);
  } while (*id == 0);
  return true;
}

// Gate a LeakMemory intervention and breadcrumb it into crash-report diagnostics.
inline bool SkRecordReplayEnterLeakMemory(const char* label) {
  if (!SkRecordReplayIsRecordingOrReplaying("leak-references", label))
    return false;
  SkRecordReplayDiagnostic("LeakMemory %s", label);
  return true;
}
extern void SkRecordReplayBeginPassThroughEvents();
extern void SkRecordReplayEndPassThroughEvents();
extern bool SkRecordReplayIsReplaying();
extern bool SkRecordReplayHasDivergedFromRecording();
extern bool SkRecordReplayAreEventsUnavailable(const char* why);
extern uintptr_t SkRecordReplayValue(const char* why, uintptr_t v);

#endif
