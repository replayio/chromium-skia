/*
 * Copyright 2025 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkSynchronizedResourceCache_DEFINED
#define SkSynchronizedResourceCache_DEFINED

#include "include/private/SkDebug.h"
#include "include/private/SkMutex.h"
#include "src/core/SkResourceCache.h"

#include <cstddef>

class SkCachedData;

class SkSynchronizedResourceCache : public SkResourceCache {
public:
    bool find(const Key& key, FindVisitor, void* context) override;
    void add(Rec*, void* payload = nullptr) override;

    void visitAll(Visitor, void* context) override;

    size_t getTotalBytesUsed() const override;
    size_t getTotalByteLimit() const override;
    size_t setTotalByteLimit(size_t newLimit) override;

    size_t setSingleAllocationByteLimit(size_t) override;
    size_t getSingleAllocationByteLimit() const override;
    size_t getEffectiveSingleAllocationByteLimit() const override;

    void purgeAll() override;

    DiscardableFactory discardableFactory() const override;

    SkCachedData* newCachedData(size_t bytes) override;

    void dump() const override;

    SkSynchronizedResourceCache(DiscardableFactory);
    explicit SkSynchronizedResourceCache(size_t byteLimit);
    ~SkSynchronizedResourceCache() override;

private:
    // Named, so taking it is an ordered lock: SkGraphics::Init makes the global cache this
    // guards, and threads find in it what the recording's threads found. A cache owned by an
    // SkSharedContext gets its own ordered lock wherever that context is made, which nothing
    // pins; no Chromium build makes one today.
    mutable SkMutex fMutex{"SkResourceCache"};
};
#endif
