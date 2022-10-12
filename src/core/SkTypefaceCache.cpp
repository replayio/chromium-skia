/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/private/SkMutex.h"
#include "src/core/SkTypefaceCache.h"
#include <atomic>

#include "src/core/SkRecordReplay.h"

#define TYPEFACE_CACHE_LIMIT    1024

SkTypefaceCache::SkTypefaceCache() {}

void SkTypefaceCache::add(sk_sp<SkTypeface> face) {
    if (fTypefaces.count() >= TYPEFACE_CACHE_LIMIT) {
        this->purge(TYPEFACE_CACHE_LIMIT >> 2);
    }

    SkRecordReplayAssert("[RUN-2612-2639] SkTypefaceCache::add %u", face->uniqueID());

    fTypefaces.emplace_back(std::move(face));
}

sk_sp<SkTypeface> SkTypefaceCache::findByProcAndRef(FindProc proc, void* ctx) const {
    SkRecordReplayAssert("[RUN-2612-2639] SkTypefaceCache::findByProcAndRef A %d", fTypefaces.count());
    for (const sk_sp<SkTypeface>& typeface : fTypefaces) {
        if (proc(typeface.get(), ctx)) {
            SkRecordReplayAssert("[RUN-2612-2639] SkTypefaceCache::findByProcAndRef B %u",
                                 typeface->uniqueID());
            return typeface;
        }
    }
    SkRecordReplayAssert("[RUN-2612-2639] SkTypefaceCache::findByProcAndRef C");
    return nullptr;
}

void SkTypefaceCache::purge(int numToPurge) {
    int count = fTypefaces.count();
    int i = 0;
    SkRecordReplayAssert("[RUN-2612-2639] SkTypefaceCache::purge %d %d", count, numToPurge);
    while (i < count) {
        if (fTypefaces[i]->unique()) {
            fTypefaces.removeShuffle(i);
            --count;
            if (--numToPurge == 0) {
                return;
            }
        } else {
            ++i;
        }
    }
}

void SkTypefaceCache::purgeAll() {
    this->purge(fTypefaces.count());
}

///////////////////////////////////////////////////////////////////////////////

SkTypefaceCache& SkTypefaceCache::Get() {
    static SkTypefaceCache gCache;
    return gCache;
}

SkTypefaceID SkTypefaceCache::NewTypefaceID() {
    static std::atomic<int32_t> nextID{1};
    return nextID.fetch_add(1, std::memory_order_relaxed);
}

static SkMutex& typeface_cache_mutex() {
    static SkMutex& mutex = *(new SkMutex);
    return mutex;
}

void SkTypefaceCache::Add(sk_sp<SkTypeface> face) {
    SkAutoMutexExclusive ama(typeface_cache_mutex());
    Get().add(std::move(face));
}

sk_sp<SkTypeface> SkTypefaceCache::FindByProcAndRef(FindProc proc, void* ctx) {
    SkAutoMutexExclusive ama(typeface_cache_mutex());
    return Get().findByProcAndRef(proc, ctx);
}

void SkTypefaceCache::PurgeAll() {
    SkAutoMutexExclusive ama(typeface_cache_mutex());
    Get().purgeAll();
}

///////////////////////////////////////////////////////////////////////////////

#ifdef SK_DEBUG
static bool DumpProc(SkTypeface* face, void* ctx) {
    SkString n;
    face->getFamilyName(&n);
    SkFontStyle s = face->fontStyle();
    SkTypefaceID id = face->uniqueID();
    SkDebugf("SkTypefaceCache: face %p typefaceID %d weight %d width %d style %d name %s\n",
             face, id, s.weight(), s.width(), s.slant(), n.c_str());
    return false;
}
#endif

void SkTypefaceCache::Dump() {
#ifdef SK_DEBUG
    (void)Get().findByProcAndRef(DumpProc, nullptr);
#endif
}
