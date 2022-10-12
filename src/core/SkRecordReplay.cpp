/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/private/SkOnce.h"
#include "SkRecordReplay.h"

#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif

#include <stdarg.h>
#include <string.h>

#include <string>


// ##########################################################################
// Driver/Linker API
// ##########################################################################
static bool gRecordingOrReplaying;
static bool gHasDisabledFeatures;

static void (*gRecordReplayPrint)(const char* format, va_list args);
static void (*gRecordReplayWarning)(const char* format, va_list args);
static void (*gRecordReplayAssert)(const char*, va_list);
static void (*gRecordReplayDiagnostic)(const char*, va_list);
static void (*gRecordReplayRegisterPointer)(const void* ptr);
static void (*gRecordReplayUnregisterPointer)(const void* ptr);
static int (*gRecordReplayPointerId)(const void* ptr);

static bool (*gRecordReplayHasDisabledFeatures)();
static bool (*gRecordReplayFeatureEnabled)(const char* feature, const char* subfeature);
static bool (*gRecordReplayAreEventsDisallowed)();
static void (*gRecordReplayBeginPassThroughEvents)();
static void (*gRecordReplayEndPassThroughEvents)();
static bool (*gRecordReplayIsReplaying)(void);
static bool (*gRecordReplayHasDivergedFromRecording)(void);
static uintptr_t (*gRecordReplayValue)(const char* why, uintptr_t v);

template <typename Src, typename Dst>
static inline void CastPointer(const Src src, Dst* dst) {
  static_assert(sizeof(Src) == sizeof(void*), "bad size");
  static_assert(sizeof(Dst) == sizeof(void*), "bad size");
  memcpy((void*)dst, (const void*)&src, sizeof(void*));
}

template <typename T>
static void RecordReplayLoadSymbol(const char* name, T& function) {
#ifndef _WIN32
  void* sym = dlsym(RTLD_DEFAULT, name);
#else
  HMODULE module = GetModuleHandleA("windows-recordreplay.dll");
  void* sym = module ? (void*)GetProcAddress(module, name) : nullptr;
#endif
  if (sym) {
    CastPointer(sym, &function);
  }
}

static inline bool EnsureInitialized() {
  static SkOnce once;
  once([]{
    RecordReplayLoadSymbol("RecordReplayPrint", gRecordReplayPrint);
    RecordReplayLoadSymbol("RecordReplayWarning", gRecordReplayWarning);
    RecordReplayLoadSymbol("RecordReplayAssert", gRecordReplayAssert);
    RecordReplayLoadSymbol("RecordReplayDiagnostic", gRecordReplayDiagnostic);
    RecordReplayLoadSymbol("RecordReplayRegisterPointer", gRecordReplayRegisterPointer);
    RecordReplayLoadSymbol("RecordReplayUnregisterPointer", gRecordReplayUnregisterPointer);
    RecordReplayLoadSymbol("RecordReplayPointerId", gRecordReplayPointerId);

    RecordReplayLoadSymbol("RecordReplayHasDisabledFeatures", gRecordReplayHasDisabledFeatures);
    RecordReplayLoadSymbol("RecordReplayFeatureEnabled", gRecordReplayFeatureEnabled);
    RecordReplayLoadSymbol("RecordReplayAreEventsDisallowed", gRecordReplayAreEventsDisallowed);
    RecordReplayLoadSymbol("RecordReplayBeginPassThroughEvents", gRecordReplayBeginPassThroughEvents);
    RecordReplayLoadSymbol("RecordReplayEndPassThroughEvents", gRecordReplayEndPassThroughEvents);
    RecordReplayLoadSymbol("RecordReplayIsReplaying", gRecordReplayIsReplaying);
    RecordReplayLoadSymbol("RecordReplayHasDivergedFromRecording", gRecordReplayHasDivergedFromRecording);
    RecordReplayLoadSymbol("RecordReplayValue", gRecordReplayValue);

    gRecordingOrReplaying =
            gRecordReplayFeatureEnabled && gRecordReplayFeatureEnabled("record-replay", nullptr);
    gHasDisabledFeatures = gRecordingOrReplaying && gRecordReplayHasDisabledFeatures();
  });
  return !!gRecordReplayAssert;
}

void SkRecordReplayPrint(const char* format, ...) {
  if (SkRecordReplayIsRecordingOrReplaying()) {
    va_list args;
    va_start(args, format);
    gRecordReplayPrint(format, args);
    va_end(args);
  }
}

void SkRecordReplayWarning(const char* format, ...) {
  if (EnsureInitialized()) {
    va_list ap;
    va_start(ap, format);
    gRecordReplayWarning(format, ap);
    va_end(ap);
  }
}

void SkRecordReplayAssert(const char* format, ...) {
  if (EnsureInitialized()) {
    va_list ap;
    va_start(ap, format);
    gRecordReplayAssert(format, ap);
    va_end(ap);
  }
}

void SkRecordReplayDiagnostic(const char* format, ...) {
  if (EnsureInitialized()) {
    va_list ap;
    va_start(ap, format);
    gRecordReplayDiagnostic(format, ap);
    va_end(ap);
  }
}

void SkRecordReplayRegisterPointer(const void* ptr) {
  if (EnsureInitialized()) {
    gRecordReplayRegisterPointer(ptr);
  }
}

void SkRecordReplayUnregisterPointer(const void* ptr) {
  if (EnsureInitialized()) {
    gRecordReplayUnregisterPointer(ptr);
  }
}

bool SkRecordReplayFeatureEnabled(const char* feature, const char* subfeature) {
  if (EnsureInitialized()) {
    if (!gHasDisabledFeatures) {
        return true;
    }
    return gRecordReplayFeatureEnabled(feature, subfeature);
  }
  return true;
}

bool SkRecordReplayIsRecordingOrReplaying(const char* feature, const char* subfeature) {
  return EnsureInitialized() && gRecordingOrReplaying &&
    (!feature || SkRecordReplayFeatureEnabled(feature, subfeature));
}

bool SkRecordReplayAreEventsDisallowed(const char* why) {
  if (EnsureInitialized()) {
    if (SkRecordReplayIsRecordingOrReplaying("disallow-events", why)) {
      return gRecordReplayAreEventsDisallowed();
    }
  }
  return false;
}

void SkRecordReplayBeginPassThroughEvents() {
  if (EnsureInitialized()) {
    gRecordReplayBeginPassThroughEvents();
  }
}

void SkRecordReplayEndPassThroughEvents() {
  if (EnsureInitialized()) {
    gRecordReplayEndPassThroughEvents();
  }
}

bool SkRecordReplayIsReplaying(void) {
  return EnsureInitialized() && gRecordReplayIsReplaying();
}

bool SkRecordReplayHasDivergedFromRecording(void) {
  return EnsureInitialized() && 
    gRecordReplayIsReplaying() && 
    gRecordReplayHasDivergedFromRecording();
}

bool SkRecordReplayAreEventsUnavailable(const char* why) {
  return SkRecordReplayAreEventsDisallowed(why) ||
    SkRecordReplayHasDivergedFromRecording();
}

uintptr_t SkRecordReplayValue(const char* why, uintptr_t v) {
  if (SkRecordReplayIsRecordingOrReplaying("values", why)) {
    return gRecordReplayValue(why, v);
  }
  return v;
}