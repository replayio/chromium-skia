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

// A mutex made in a process that's being recorded or replayed, while its events are recorded, is
// taken by threads in the same order when replaying as when recording: Skia's caches are shared
// between threads, and what a thread finds in one decides what it does next. Nonzero is the
// driver's ordered lock for a mutex made now without a name, which then stands in for the
// semaphore; zero where nothing is recorded.
extern int SkRecordReplayNewMutexId();
extern int SkRecordReplayCreateOrderedLock(const char* ordered_name);
extern void SkRecordReplayOrderedLock(int lock);
extern void SkRecordReplayOrderedUnlock(int lock);

class SK_CAPABILITY("mutex") SkMutex {
public:
    SkMutex() : fOrderedLockId(SkRecordReplayNewMutexId()) {}
    SkMutex(const char* ordered_name) {
      fOrderedLockId = SkRecordReplayCreateOrderedLock(ordered_name);
    }

    ~SkMutex() {
        this->assertNotHeld();
    }

    void acquire() SK_ACQUIRE() {
        if (fOrderedLockId) {
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
