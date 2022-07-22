/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkColorFilter.h"
#include "include/core/SkFlattenable.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypes.h"
#include "src/core/SkColorFilterBase.h"
#include "src/core/SkColorFilterPriv.h"
#include "src/core/SkEffectPriv.h"
#include "src/core/SkRasterPipeline.h"
#include "src/core/SkVM.h"

#if SK_SUPPORT_GPU
#include "src/gpu/ganesh/GrFragmentProcessor.h"

#include <memory>
#include <utility>

class GrColorInfo;
class GrRecordingContext;
#endif

class SkArenaAlloc;
class SkColorInfo;
class SkReadBuffer;
class SkWriteBuffer;

#ifdef SK_ENABLE_SKSL
class SkPipelineDataGatherer;
#endif

/**
 * Remaps the input color's alpha to a Gaussian ramp and then outputs premul white using the
 * remapped alpha.
 */
class SkGaussianColorFilter : public SkColorFilterBase {
public:
    SkGaussianColorFilter() : SkColorFilterBase() {}

#if SK_SUPPORT_GPU
    GrFPResult asFragmentProcessor(std::unique_ptr<GrFragmentProcessor> inputFP,
                                   GrRecordingContext*,
                                   const GrColorInfo&) const override;
#endif

#ifdef SK_ENABLE_SKSL
    void addToKey(const SkKeyContext& keyContext,
                  SkPaintParamsKeyBuilder* builder,
                  SkPipelineDataGatherer* gatherer) const override;
#endif

protected:
    void flatten(SkWriteBuffer&) const override {}
    bool onAppendStages(const SkStageRec& rec, bool shaderIsOpaque) const override {
        rec.fPipeline->append(SkRasterPipeline::gauss_a_to_rgba);
        return true;
    }

    skvm::Color onProgram(skvm::Builder* p, skvm::Color c, const SkColorInfo& dst, skvm::Uniforms*,
                          SkArenaAlloc*) const override {
        // x = 1 - x;
        // exp(-x * x * 4) - 0.018f;
        // ... now approximate with quartic
        //
        skvm::F32 x = p->splat(-2.26661229133605957031f);
        x = c.a * x + 2.89795351028442382812f;
        x = c.a * x + 0.21345567703247070312f;
        x = c.a * x + 0.15489584207534790039f;
        x = c.a * x + 0.00030726194381713867f;
        return {x, x, x, x};
    }

private:
    SK_FLATTENABLE_HOOKS(SkGaussianColorFilter)
};

sk_sp<SkFlattenable> SkGaussianColorFilter::CreateProc(SkReadBuffer&) {
    return SkColorFilterPriv::MakeGaussian();
}

#if SK_SUPPORT_GPU

#include "include/effects/SkRuntimeEffect.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/gpu/ganesh/effects/GrSkSLFP.h"

GrFPResult SkGaussianColorFilter::asFragmentProcessor(std::unique_ptr<GrFragmentProcessor> inputFP,
                                                      GrRecordingContext*,
                                                      const GrColorInfo&) const {
    static const SkRuntimeEffect* effect = SkMakeRuntimeEffect(SkRuntimeEffect::MakeForColorFilter,
        R"(
            half4 main(half4 inColor) {
                half factor = 1 - inColor.a;
                factor = exp(-factor * factor * 4) - 0.018;
                return half4(factor);
            }
        )");
    SkASSERT(SkRuntimeEffectPriv::SupportsConstantOutputForConstantInput(effect));
    return GrFPSuccess(GrSkSLFP::Make(effect, "gaussian_fp", std::move(inputFP),
                                      GrSkSLFP::OptFlags::kNone));
}
#endif

#ifdef SK_ENABLE_SKSL

#include "src/core/SkKeyContext.h"
#include "src/core/SkKeyHelpers.h"
#include "src/core/SkPaintParamsKey.h"

void SkGaussianColorFilter::addToKey(const SkKeyContext& keyContext,
                                     SkPaintParamsKeyBuilder* builder,
                                     SkPipelineDataGatherer* gatherer) const {
    GaussianColorFilterBlock::BeginBlock(keyContext, builder, gatherer);
    builder->endBlock();
}

#endif

sk_sp<SkColorFilter> SkColorFilterPriv::MakeGaussian() {
    return sk_sp<SkColorFilter>(new SkGaussianColorFilter);
}
