/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkMutex_DEFINED
#define SkMutex_DEFINED

#include "include/core/SkTypes.h"
#include "include/private/SkMacros.h"
#include "include/private/SkSemaphore.h"
#include "include/private/SkThreadAnnotations.h"
#include "include/private/SkThreadID.h"

extern int SkRecordReplayCreateOrderedLock(const char* ordered_name);
extern void SkRecordReplayOrderedLock(int lock);
extern void SkRecordReplayOrderedUnlock(int lock);

class SK_CAPABILITY("mutex") SkMutex {
public:
    constexpr SkMutex() = default;
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
