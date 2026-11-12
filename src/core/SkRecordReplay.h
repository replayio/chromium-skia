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

// Checks of what isolated draws touch, enabled by
// RECORD_REPLAY_ISOLATED_DRAW_ASSERTS: report |what| in an isolated draw, or,
// for the second, outside one. Callers are on paths that are already slow
// (ordered locks, new typeface IDs, IDs taken with events disallowed).
extern void SkRecordReplayIsolatedDrawAssert(const char* what, const void* address);
extern void SkRecordReplayDisallowedIdAssert(const char* what, const void* address);

// Code that runs with recorded events disallowed takes IDs from ranges of its
// own: an ID it took from a shared counter would be given again to an object
// a recorded thread creates later, and some of that code (replay-only
// commands, unordered tasks) doesn't run at the same points when recording
// and replaying. Isolated draws, which run only when replaying, have a range
// apart from the rest of that code, whose IDs would otherwise differ between
// recording and replaying once a draw ran. Both count down, isolated draws
// from the largest value and other code from half of it, while shared
// counters count up, so they would only meet where a counter wraps. Takes
// multiples of |Step| greater than |Reserved|. Sets |id| and returns true if
// events are disallowed. IDs taken lazily, such as pixel ref and path
// generation IDs, come from these ranges also for objects that recorded
// threads use later.
template <typename T, T Step = 1, T Reserved = 0>
inline bool SkRecordReplayTakeIsolatedId(T* id) {
  if (!SkRecordReplayAreEventsDisallowedByDriver()) {
    return false;
  }
  SkRecordReplayDisallowedIdAssert("Skia ID", __builtin_return_address(0));
  constexpr T kTop = std::numeric_limits<T>::max() / Step * Step;
  static std::atomic<T> drawNext{kTop};
  static std::atomic<T> disallowedNext{kTop / 2 / Step * Step};
  std::atomic<T>& next = SkRecordReplayInIsolatedDraw() ? drawNext : disallowedNext;
  do {
    *id = next.fetch_sub(Step, std::memory_order_relaxed);
  } while (*id <= Reserved);
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
