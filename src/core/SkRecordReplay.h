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

// Code that runs with recorded events disallowed, such as Replay's in-process
// draws while replaying, takes IDs from a range of its own: an ID it took from
// a shared counter would be given again to an object a recorded thread creates
// later. Its IDs count down from the largest value, and shared counters count
// up, so they would only meet where a counter wraps. Never takes 0. Sets |id|
// and returns true if events are disallowed.
template <typename T>
inline bool SkRecordReplayTakeIsolatedId(T* id) {
  if (!SkRecordReplayAreEventsDisallowedByDriver()) {
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
