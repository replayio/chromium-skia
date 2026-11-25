/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkMutex_DEFINED
#define SkMutex_DEFINED

#include "include/private/SkAssert.h"
#include "include/private/SkDebug.h"
#include "include/private/SkSemaphore.h"
#include "include/private/SkThreadAnnotations.h"
#include "include/private/SkThreadID.h"

/**
 * class SkMutex
 *
 * This allows us to have a mutex without needing the one in
 * the C++ std library which does not work with all clients.
 * go/cstyle#Disallowed_Stdlib
 */

// A mutex given a name is taken by threads in the same order when replaying as when recording,
// which is what the shared caches behind those names need: what a thread finds in one decides
// what it does next. Ordering is opt-in by name, as it is for Chromium's base::Lock, and a mutex
// made without a name keeps its semaphore. The driver records which thread acquires an ordered
// lock, so ordering a mutex that threads reach by racing, holding no lock of their own, would
// leave a replay waiting for a thread that never arrives.
extern int SkRecordReplayCreateOrderedLock(const char* ordered_name);
extern void SkRecordReplayOrderedLock(int lock);
extern void SkRecordReplayOrderedUnlock(int lock);
extern void SkRecordReplayIsolatedDrawAssert(const char* what, const void* address);

class SK_CAPABILITY("mutex") SkMutex {
public:
    constexpr SkMutex() = default;
    SkMutex(const char* ordered_name) {
      if (ordered_name) {
        fOrderedLockId = SkRecordReplayCreateOrderedLock(ordered_name);
      }
    }

    ~SkMutex() {
        this->assertNotHeld();
    }

    void acquire() SK_ACQUIRE() {
        if (fOrderedLockId) {
          SkRecordReplayIsolatedDrawAssert("ordered SkMutex", __builtin_return_address(0));
          SkRecordReplayOrderedLock(fOrderedLockId);
        } else {
          fSemaphore.wait();
        }
        SkDEBUGCODE(fOwner = SkGetThreadID();)
    }

    void release() SK_RELEASE_CAPABILITY() {
        this->assertHeld();
        SkDEBUGCODE(fOwner = kIllegalThreadID;)
        if (fOrderedLockId) {
          SkRecordReplayOrderedUnlock(fOrderedLockId);
        } else {
          fSemaphore.signal();
        }
    }

    void assertHeld() SK_ASSERT_CAPABILITY(this) {
        SkASSERT(fOwner == SkGetThreadID());
    }

    void assertNotHeld() {
        SkASSERT(fOwner == kIllegalThreadID);
    }

private:
    SkSemaphore fSemaphore{1};
    int fOrderedLockId = 0;
    SkDEBUGCODE(SkThreadID fOwner{kIllegalThreadID};)
};

class SK_SCOPED_CAPABILITY SkAutoMutexExclusive {
public:
    SkAutoMutexExclusive(SkMutex& mutex) SK_ACQUIRE(mutex) : fMutex(mutex) { fMutex.acquire(); }
    ~SkAutoMutexExclusive() SK_RELEASE_CAPABILITY() { fMutex.release(); }

    SkAutoMutexExclusive(const SkAutoMutexExclusive&) = delete;
    SkAutoMutexExclusive(SkAutoMutexExclusive&&) = delete;

    SkAutoMutexExclusive& operator=(const SkAutoMutexExclusive&) = delete;
    SkAutoMutexExclusive& operator=(SkAutoMutexExclusive&&) = delete;

private:
    SkMutex& fMutex;
};

#endif  // SkMutex_DEFINED
