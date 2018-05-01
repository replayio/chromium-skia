/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "Skottie.h"

#include "SkCanvas.h"
#include "SkJSONCPP.h"
#include "SkottieAdapter.h"
#include "SkottieAnimator.h"
#include "SkottieParser.h"
#include "SkottieValue.h"
#include "SkData.h"
#include "SkImage.h"
#include "SkMakeUnique.h"
#include "SkOSPath.h"
#include "SkPaint.h"
#include "SkParse.h"
#include "SkPoint.h"
#include "SkSGClipEffect.h"
#include "SkSGColor.h"
#include "SkSGDraw.h"
#include "SkSGGeometryTransform.h"
#include "SkSGGradient.h"
#include "SkSGGroup.h"
#include "SkSGImage.h"
#include "SkSGInvalidationController.h"
#include "SkSGMaskEffect.h"
#include "SkSGMerge.h"
#include "SkSGOpacityEffect.h"
#include "SkSGPath.h"
#include "SkSGRect.h"
#include "SkSGRoundEffect.h"
#include "SkSGScene.h"
#include "SkSGTransform.h"
#include "SkSGTrimEffect.h"
#include "SkStream.h"
#include "SkTArray.h"
#include "SkTime.h"
#include "SkTHash.h"

#include <cmath>
#include <vector>

#include "stdlib.h"

namespace skottie {

#define LOG SkDebugf

namespace {

using AssetMap = SkTHashMap<SkString, const Json::Value*>;

struct AttachContext {
    const ResourceProvider& fResources;
    const AssetMap&         fAssets;
    const float             fFrameRate;
    sksg::AnimatorList&     fAnimators;
};

bool LogFail(const Json::Value& json, const char* msg) {
    const auto dump = json.toStyledString();
    LOG("!! %s: %s", msg, dump.c_str());
    return false;
}

sk_sp<sksg::Matrix> AttachMatrix(const Json::Value& t, AttachContext* ctx,
                                        sk_sp<sksg::Matrix> parentMatrix) {
    if (!t.isObject())
        return nullptr;

    auto matrix = sksg::Matrix::Make(SkMatrix::I(), std::move(parentMatrix));
    auto adapter = sk_make_sp<TransformAdapter>(matrix);
    auto anchor_attached = BindProperty<VectorValue>(t["a"], &ctx->fAnimators,
            [adapter](const VectorValue& a) {
                adapter->setAnchorPoint(ValueTraits<VectorValue>::As<SkPoint>(a));
            });
    auto position_attached = BindProperty<VectorValue>(t["p"], &ctx->fAnimators,
            [adapter](const VectorValue& p) {
                adapter->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
            });
    auto scale_attached = BindProperty<VectorValue>(t["s"], &ctx->fAnimators,
            [adapter](const VectorValue& s) {
                adapter->setScale(ValueTraits<VectorValue>::As<SkVector>(s));
            });

    auto* jrotation = &t["r"];
    if (jrotation->isNull()) {
        // 3d rotations have separate rx,ry,rz components.  While we don't fully support them,
        // we can still make use of rz.
        jrotation = &t["rz"];
    }
    auto rotation_attached = BindProperty<ScalarValue>(*jrotation, &ctx->fAnimators,
            [adapter](const ScalarValue& r) {
                adapter->setRotation(r);
            });
    auto skew_attached = BindProperty<ScalarValue>(t["sk"], &ctx->fAnimators,
            [adapter](const ScalarValue& sk) {
                adapter->setSkew(sk);
            });
    auto skewaxis_attached = BindProperty<ScalarValue>(t["sa"], &ctx->fAnimators,
            [adapter](const ScalarValue& sa) {
                adapter->setSkewAxis(sa);
            });

    if (!anchor_attached &&
        !position_attached &&
        !scale_attached &&
        !rotation_attached &&
        !skew_attached &&
        !skewaxis_attached) {
        LogFail(t, "Could not parse transform");
        return nullptr;
    }

    return matrix;
}

sk_sp<sksg::RenderNode> AttachOpacity(const Json::Value& jtransform, AttachContext* ctx,
                                      sk_sp<sksg::RenderNode> childNode) {
    if (!jtransform.isObject() || !childNode)
        return childNode;

    // This is more peeky than other attachers, because we want to avoid redundant opacity
    // nodes for the extremely common case of static opaciy == 100.
    const auto& opacity = jtransform["o"];
    if (opacity.isObject() &&
        !ParseDefault(opacity["a"], true) &&
        ParseDefault(opacity["k"], -1) == 100) {
        // Ignoring static full opacity.
        return childNode;
    }

    auto opacityNode = sksg::OpacityEffect::Make(childNode);
    BindProperty<ScalarValue>(opacity, &ctx->fAnimators,
        [opacityNode](const ScalarValue& o) {
            // BM opacity is [0..100]
            opacityNode->setOpacity(o * 0.01f);
        });

    return std::move(opacityNode);
}

sk_sp<sksg::RenderNode> AttachComposition(const Json::Value&, AttachContext* ctx);

sk_sp<sksg::Path> AttachPath(const Json::Value& jpath, AttachContext* ctx) {
    auto path_node = sksg::Path::Make();
    return BindProperty<ShapeValue>(jpath, &ctx->fAnimators,
        [path_node](const ShapeValue& p) {
            path_node->setPath(ValueTraits<ShapeValue>::As<SkPath>(p));
        })
        ? path_node
        : nullptr;
}

sk_sp<sksg::GeometryNode> AttachPathGeometry(const Json::Value& jpath, AttachContext* ctx) {
    SkASSERT(jpath.isObject());

    return AttachPath(jpath["ks"], ctx);
}

sk_sp<sksg::GeometryNode> AttachRRectGeometry(const Json::Value& jrect, AttachContext* ctx) {
    SkASSERT(jrect.isObject());

    auto rect_node = sksg::RRect::Make();
    auto adapter = sk_make_sp<RRectAdapter>(rect_node);

    auto p_attached = BindProperty<VectorValue>(jrect["p"], &ctx->fAnimators,
        [adapter](const VectorValue& p) {
                adapter->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
        });
    auto s_attached = BindProperty<VectorValue>(jrect["s"], &ctx->fAnimators,
        [adapter](const VectorValue& s) {
            adapter->setSize(ValueTraits<VectorValue>::As<SkSize>(s));
        });
    auto r_attached = BindProperty<ScalarValue>(jrect["r"], &ctx->fAnimators,
        [adapter](const ScalarValue& r) {
            adapter->setRadius(SkSize::Make(r, r));
        });

    if (!p_attached && !s_attached && !r_attached) {
        return nullptr;
    }

    return std::move(rect_node);
}

sk_sp<sksg::GeometryNode> AttachEllipseGeometry(const Json::Value& jellipse, AttachContext* ctx) {
    SkASSERT(jellipse.isObject());

    auto rect_node = sksg::RRect::Make();
    auto adapter = sk_make_sp<RRectAdapter>(rect_node);

    auto p_attached = BindProperty<VectorValue>(jellipse["p"], &ctx->fAnimators,
        [adapter](const VectorValue& p) {
            adapter->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
        });
    auto s_attached = BindProperty<VectorValue>(jellipse["s"], &ctx->fAnimators,
        [adapter](const VectorValue& s) {
            const auto sz = ValueTraits<VectorValue>::As<SkSize>(s);
            adapter->setSize(sz);
            adapter->setRadius(SkSize::Make(sz.width() / 2, sz.height() / 2));
        });

    if (!p_attached && !s_attached) {
        return nullptr;
    }

    return std::move(rect_node);
}

sk_sp<sksg::GeometryNode> AttachPolystarGeometry(const Json::Value& jstar, AttachContext* ctx) {
    SkASSERT(jstar.isObject());

    static constexpr PolyStarAdapter::Type gTypes[] = {
        PolyStarAdapter::Type::kStar, // "sy": 1
        PolyStarAdapter::Type::kPoly, // "sy": 2
    };

    const auto type = ParseDefault(jstar["sy"], 0) - 1;
    if (type < 0 || type >= SkTo<int>(SK_ARRAY_COUNT(gTypes))) {
        LogFail(jstar, "Unknown polystar type");
        return nullptr;
    }

    auto path_node = sksg::Path::Make();
    auto adapter = sk_make_sp<PolyStarAdapter>(path_node, gTypes[type]);

    BindProperty<VectorValue>(jstar["p"], &ctx->fAnimators,
        [adapter](const VectorValue& p) {
            adapter->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
        });
    BindProperty<ScalarValue>(jstar["pt"], &ctx->fAnimators,
        [adapter](const ScalarValue& pt) {
            adapter->setPointCount(pt);
        });
    BindProperty<ScalarValue>(jstar["ir"], &ctx->fAnimators,
        [adapter](const ScalarValue& ir) {
            adapter->setInnerRadius(ir);
        });
    BindProperty<ScalarValue>(jstar["or"], &ctx->fAnimators,
        [adapter](const ScalarValue& otr) {
            adapter->setOuterRadius(otr);
        });
    BindProperty<ScalarValue>(jstar["is"], &ctx->fAnimators,
        [adapter](const ScalarValue& is) {
            adapter->setInnerRoundness(is);
        });
    BindProperty<ScalarValue>(jstar["os"], &ctx->fAnimators,
        [adapter](const ScalarValue& os) {
            adapter->setOuterRoundness(os);
        });
    BindProperty<ScalarValue>(jstar["r"], &ctx->fAnimators,
        [adapter](const ScalarValue& r) {
            adapter->setRotation(r);
        });

    return std::move(path_node);
}

sk_sp<sksg::Color> AttachColor(const Json::Value& obj, AttachContext* ctx) {
    SkASSERT(obj.isObject());

    auto color_node = sksg::Color::Make(SK_ColorBLACK);
    auto color_attached = BindProperty<VectorValue>(obj["c"], &ctx->fAnimators,
        [color_node](const VectorValue& c) {
            color_node->setColor(ValueTraits<VectorValue>::As<SkColor>(c));
        });

    return color_attached ? color_node : nullptr;
}

sk_sp<sksg::Gradient> AttachGradient(const Json::Value& obj, AttachContext* ctx) {
    SkASSERT(obj.isObject());

    const auto& stops = obj["g"];
    if (!stops.isObject())
        return nullptr;

    const auto stopCount = ParseDefault(stops["p"], -1);
    if (stopCount < 0)
        return nullptr;

    sk_sp<sksg::Gradient> gradient_node;
    sk_sp<GradientAdapter> adapter;

    if (ParseDefault(obj["t"], 1) == 1) {
        auto linear_node = sksg::LinearGradient::Make();
        adapter = sk_make_sp<LinearGradientAdapter>(linear_node, stopCount);
        gradient_node = std::move(linear_node);
    } else {
        auto radial_node = sksg::RadialGradient::Make();
        adapter = sk_make_sp<RadialGradientAdapter>(radial_node, stopCount);

        // TODO: highlight, angle
        gradient_node = std::move(radial_node);
    }

    BindProperty<VectorValue>(stops["k"], &ctx->fAnimators,
        [adapter](const VectorValue& stops) {
            adapter->setColorStops(stops);
        });
    BindProperty<VectorValue>(obj["s"], &ctx->fAnimators,
        [adapter](const VectorValue& s) {
            adapter->setStartPoint(ValueTraits<VectorValue>::As<SkPoint>(s));
        });
    BindProperty<VectorValue>(obj["e"], &ctx->fAnimators,
        [adapter](const VectorValue& e) {
            adapter->setEndPoint(ValueTraits<VectorValue>::As<SkPoint>(e));
        });

    return gradient_node;
}

sk_sp<sksg::PaintNode> AttachPaint(const Json::Value& jpaint, AttachContext* ctx,
                                   sk_sp<sksg::PaintNode> paint_node) {
    if (paint_node) {
        paint_node->setAntiAlias(true);

        BindProperty<ScalarValue>(jpaint["o"], &ctx->fAnimators,
            [paint_node](const ScalarValue& o) {
                // BM opacity is [0..100]
                paint_node->setOpacity(o * 0.01f);
        });
    }

    return paint_node;
}

sk_sp<sksg::PaintNode> AttachStroke(const Json::Value& jstroke, AttachContext* ctx,
                                    sk_sp<sksg::PaintNode> stroke_node) {
    SkASSERT(jstroke.isObject());

    if (!stroke_node)
        return nullptr;

    stroke_node->setStyle(SkPaint::kStroke_Style);

    auto width_attached = BindProperty<ScalarValue>(jstroke["w"], &ctx->fAnimators,
        [stroke_node](const ScalarValue& w) {
            stroke_node->setStrokeWidth(w);
        });
    if (!width_attached)
        return nullptr;

    stroke_node->setStrokeMiter(ParseDefault(jstroke["ml"], 4.0f));

    static constexpr SkPaint::Join gJoins[] = {
        SkPaint::kMiter_Join,
        SkPaint::kRound_Join,
        SkPaint::kBevel_Join,
    };
    stroke_node->setStrokeJoin(gJoins[SkTPin<int>(ParseDefault(jstroke["lj"], 1) - 1,
                                                  0, SK_ARRAY_COUNT(gJoins) - 1)]);

    static constexpr SkPaint::Cap gCaps[] = {
        SkPaint::kButt_Cap,
        SkPaint::kRound_Cap,
        SkPaint::kSquare_Cap,
    };
    stroke_node->setStrokeCap(gCaps[SkTPin<int>(ParseDefault(jstroke["lc"], 1) - 1,
                                                0, SK_ARRAY_COUNT(gCaps) - 1)]);

    return stroke_node;
}

sk_sp<sksg::PaintNode> AttachColorFill(const Json::Value& jfill, AttachContext* ctx) {
    SkASSERT(jfill.isObject());

    return AttachPaint(jfill, ctx, AttachColor(jfill, ctx));
}

sk_sp<sksg::PaintNode> AttachGradientFill(const Json::Value& jfill, AttachContext* ctx) {
    SkASSERT(jfill.isObject());

    return AttachPaint(jfill, ctx, AttachGradient(jfill, ctx));
}

sk_sp<sksg::PaintNode> AttachColorStroke(const Json::Value& jstroke, AttachContext* ctx) {
    SkASSERT(jstroke.isObject());

    return AttachStroke(jstroke, ctx, AttachPaint(jstroke, ctx, AttachColor(jstroke, ctx)));
}

sk_sp<sksg::PaintNode> AttachGradientStroke(const Json::Value& jstroke, AttachContext* ctx) {
    SkASSERT(jstroke.isObject());

    return AttachStroke(jstroke, ctx, AttachPaint(jstroke, ctx, AttachGradient(jstroke, ctx)));
}

std::vector<sk_sp<sksg::GeometryNode>> AttachMergeGeometryEffect(
    const Json::Value& jmerge, AttachContext* ctx, std::vector<sk_sp<sksg::GeometryNode>>&& geos) {
    std::vector<sk_sp<sksg::GeometryNode>> merged;

    static constexpr sksg::Merge::Mode gModes[] = {
        sksg::Merge::Mode::kMerge,      // "mm": 1
        sksg::Merge::Mode::kUnion,      // "mm": 2
        sksg::Merge::Mode::kDifference, // "mm": 3
        sksg::Merge::Mode::kIntersect,  // "mm": 4
        sksg::Merge::Mode::kXOR      ,  // "mm": 5
    };

    const auto mode = gModes[SkTPin<int>(ParseDefault(jmerge["mm"], 1) - 1,
                                         0, SK_ARRAY_COUNT(gModes) - 1)];
    merged.push_back(sksg::Merge::Make(std::move(geos), mode));

    return merged;
}

std::vector<sk_sp<sksg::GeometryNode>> AttachTrimGeometryEffect(
    const Json::Value& jtrim, AttachContext* ctx, std::vector<sk_sp<sksg::GeometryNode>>&& geos) {

    enum class Mode {
        kMerged,   // "m": 1
        kSeparate, // "m": 2
    } gModes[] = { Mode::kMerged, Mode::kSeparate };

    const auto mode = gModes[SkTPin<int>(ParseDefault(jtrim["m"], 1) - 1,
                                         0, SK_ARRAY_COUNT(gModes) - 1)];

    std::vector<sk_sp<sksg::GeometryNode>> inputs;
    if (mode == Mode::kMerged) {
        inputs.push_back(sksg::Merge::Make(std::move(geos), sksg::Merge::Mode::kMerge));
    } else {
        inputs = std::move(geos);
    }

    std::vector<sk_sp<sksg::GeometryNode>> trimmed;
    trimmed.reserve(inputs.size());
    for (const auto& i : inputs) {
        const auto trimEffect = sksg::TrimEffect::Make(i);
        trimmed.push_back(trimEffect);

        const auto adapter = sk_make_sp<TrimEffectAdapter>(std::move(trimEffect));
        BindProperty<ScalarValue>(jtrim["s"], &ctx->fAnimators,
            [adapter](const ScalarValue& s) {
                adapter->setStart(s);
            });
        BindProperty<ScalarValue>(jtrim["e"], &ctx->fAnimators,
            [adapter](const ScalarValue& e) {
                adapter->setEnd(e);
            });
        BindProperty<ScalarValue>(jtrim["o"], &ctx->fAnimators,
            [adapter](const ScalarValue& o) {
                adapter->setOffset(o);
            });
    }

    return trimmed;
}

std::vector<sk_sp<sksg::GeometryNode>> AttachRoundGeometryEffect(
    const Json::Value& jtrim, AttachContext* ctx, std::vector<sk_sp<sksg::GeometryNode>>&& geos) {

    std::vector<sk_sp<sksg::GeometryNode>> rounded;
    rounded.reserve(geos.size());

    for (const auto& g : geos) {
        const auto roundEffect = sksg::RoundEffect::Make(std::move(g));
        rounded.push_back(roundEffect);

        BindProperty<ScalarValue>(jtrim["r"], &ctx->fAnimators,
            [roundEffect](const ScalarValue& r) {
                roundEffect->setRadius(r);
            });
    }

    return rounded;
}

using GeometryAttacherT = sk_sp<sksg::GeometryNode> (*)(const Json::Value&, AttachContext*);
static constexpr GeometryAttacherT gGeometryAttachers[] = {
    AttachPathGeometry,
    AttachRRectGeometry,
    AttachEllipseGeometry,
    AttachPolystarGeometry,
};

using PaintAttacherT = sk_sp<sksg::PaintNode> (*)(const Json::Value&, AttachContext*);
static constexpr PaintAttacherT gPaintAttachers[] = {
    AttachColorFill,
    AttachColorStroke,
    AttachGradientFill,
    AttachGradientStroke,
};

using GeometryEffectAttacherT =
    std::vector<sk_sp<sksg::GeometryNode>> (*)(const Json::Value&,
                                               AttachContext*,
                                               std::vector<sk_sp<sksg::GeometryNode>>&&);
static constexpr GeometryEffectAttacherT gGeometryEffectAttachers[] = {
    AttachMergeGeometryEffect,
    AttachTrimGeometryEffect,
    AttachRoundGeometryEffect,
};

enum class ShapeType {
    kGeometry,
    kGeometryEffect,
    kPaint,
    kGroup,
    kTransform,
};

struct ShapeInfo {
    const char* fTypeString;
    ShapeType   fShapeType;
    uint32_t    fAttacherIndex; // index into respective attacher tables
};

const ShapeInfo* FindShapeInfo(const Json::Value& shape) {
    static constexpr ShapeInfo gShapeInfo[] = {
        { "el", ShapeType::kGeometry      , 2 }, // ellipse   -> AttachEllipseGeometry
        { "fl", ShapeType::kPaint         , 0 }, // fill      -> AttachColorFill
        { "gf", ShapeType::kPaint         , 2 }, // gfill     -> AttachGradientFill
        { "gr", ShapeType::kGroup         , 0 }, // group     -> Inline handler
        { "gs", ShapeType::kPaint         , 3 }, // gstroke   -> AttachGradientStroke
        { "mm", ShapeType::kGeometryEffect, 0 }, // merge     -> AttachMergeGeometryEffect
        { "rc", ShapeType::kGeometry      , 1 }, // rrect     -> AttachRRectGeometry
        { "rd", ShapeType::kGeometryEffect, 2 }, // round     -> AttachRoundGeometryEffect
        { "sh", ShapeType::kGeometry      , 0 }, // shape     -> AttachPathGeometry
        { "sr", ShapeType::kGeometry      , 3 }, // polystar  -> AttachPolyStarGeometry
        { "st", ShapeType::kPaint         , 1 }, // stroke    -> AttachColorStroke
        { "tm", ShapeType::kGeometryEffect, 1 }, // trim      -> AttachTrimGeometryEffect
        { "tr", ShapeType::kTransform     , 0 }, // transform -> Inline handler
    };

    if (!shape.isObject())
        return nullptr;

    const auto& type = shape["ty"];
    if (!type.isString())
        return nullptr;

    const auto* info = bsearch(type.asCString(),
                               gShapeInfo,
                               SK_ARRAY_COUNT(gShapeInfo),
                               sizeof(ShapeInfo),
                               [](const void* key, const void* info) {
                                  return strcmp(static_cast<const char*>(key),
                                                static_cast<const ShapeInfo*>(info)->fTypeString);
                               });

    return static_cast<const ShapeInfo*>(info);
}

struct GeometryEffectRec {
    const Json::Value&      fJson;
    GeometryEffectAttacherT fAttach;
};

struct AttachShapeContext {
    AttachShapeContext(AttachContext* ctx,
                       std::vector<sk_sp<sksg::GeometryNode>>* geos,
                       std::vector<GeometryEffectRec>* effects,
                       size_t committedAnimators)
        : fCtx(ctx)
        , fGeometryStack(geos)
        , fGeometryEffectStack(effects)
        , fCommittedAnimators(committedAnimators) {}

    AttachContext*                          fCtx;
    std::vector<sk_sp<sksg::GeometryNode>>* fGeometryStack;
    std::vector<GeometryEffectRec>*         fGeometryEffectStack;
    size_t                                  fCommittedAnimators;
};

sk_sp<sksg::RenderNode> AttachShape(const Json::Value& jshape, AttachShapeContext* shapeCtx) {
    if (!jshape.isArray())
        return nullptr;

    SkDEBUGCODE(const auto initialGeometryEffects = shapeCtx->fGeometryEffectStack->size();)

    sk_sp<sksg::Group> shape_group = sksg::Group::Make();
    sk_sp<sksg::RenderNode> shape_wrapper = shape_group;
    sk_sp<sksg::Matrix> shape_matrix;

    struct ShapeRec {
        const Json::Value& fJson;
        const ShapeInfo&   fInfo;
    };

    // First pass (bottom->top):
    //
    //   * pick up the group transform and opacity
    //   * push local geometry effects onto the stack
    //   * store recs for next pass
    //
    std::vector<ShapeRec> recs;
    for (Json::ArrayIndex i = 0; i < jshape.size(); ++i) {
        const auto& s = jshape[jshape.size() - 1 - i];
        const auto* info = FindShapeInfo(s);
        if (!info) {
            LogFail(s.isObject() ? s["ty"] : s, "Unknown shape");
            continue;
        }

        recs.push_back({ s, *info });

        switch (info->fShapeType) {
        case ShapeType::kTransform:
            if ((shape_matrix = AttachMatrix(s, shapeCtx->fCtx, nullptr))) {
                shape_wrapper = sksg::Transform::Make(std::move(shape_wrapper), shape_matrix);
            }
            shape_wrapper = AttachOpacity(s, shapeCtx->fCtx, std::move(shape_wrapper));
            break;
        case ShapeType::kGeometryEffect:
            SkASSERT(info->fAttacherIndex < SK_ARRAY_COUNT(gGeometryEffectAttachers));
            shapeCtx->fGeometryEffectStack->push_back(
                { s, gGeometryEffectAttachers[info->fAttacherIndex] });
            break;
        default:
            break;
        }
    }

    // Second pass (top -> bottom, after 2x reverse):
    //
    //   * track local geometry
    //   * emit local paints
    //
    std::vector<sk_sp<sksg::GeometryNode>> geos;
    std::vector<sk_sp<sksg::RenderNode  >> draws;
    for (auto rec = recs.rbegin(); rec != recs.rend(); ++rec) {
        switch (rec->fInfo.fShapeType) {
        case ShapeType::kGeometry: {
            SkASSERT(rec->fInfo.fAttacherIndex < SK_ARRAY_COUNT(gGeometryAttachers));
            if (auto geo = gGeometryAttachers[rec->fInfo.fAttacherIndex](rec->fJson,
                                                                         shapeCtx->fCtx)) {
                geos.push_back(std::move(geo));
            }
        } break;
        case ShapeType::kGeometryEffect: {
            // Apply the current effect and pop from the stack.
            SkASSERT(rec->fInfo.fAttacherIndex < SK_ARRAY_COUNT(gGeometryEffectAttachers));
            if (!geos.empty()) {
                geos = gGeometryEffectAttachers[rec->fInfo.fAttacherIndex](rec->fJson,
                                                                           shapeCtx->fCtx,
                                                                           std::move(geos));
            }

            SkASSERT(shapeCtx->fGeometryEffectStack->back().fJson == rec->fJson);
            SkASSERT(shapeCtx->fGeometryEffectStack->back().fAttach ==
                     gGeometryEffectAttachers[rec->fInfo.fAttacherIndex]);
            shapeCtx->fGeometryEffectStack->pop_back();
        } break;
        case ShapeType::kGroup: {
            AttachShapeContext groupShapeCtx(shapeCtx->fCtx,
                                             &geos,
                                             shapeCtx->fGeometryEffectStack,
                                             shapeCtx->fCommittedAnimators);
            if (auto subgroup = AttachShape(rec->fJson["it"], &groupShapeCtx)) {
                draws.push_back(std::move(subgroup));
                SkASSERT(groupShapeCtx.fCommittedAnimators >= shapeCtx->fCommittedAnimators);
                shapeCtx->fCommittedAnimators = groupShapeCtx.fCommittedAnimators;
            }
        } break;
        case ShapeType::kPaint: {
            SkASSERT(rec->fInfo.fAttacherIndex < SK_ARRAY_COUNT(gPaintAttachers));
            auto paint = gPaintAttachers[rec->fInfo.fAttacherIndex](rec->fJson, shapeCtx->fCtx);
            if (!paint || geos.empty())
                break;

            auto drawGeos = geos;

            // Apply all pending effects from the stack.
            for (auto it = shapeCtx->fGeometryEffectStack->rbegin();
                 it != shapeCtx->fGeometryEffectStack->rend(); ++it) {
                drawGeos = it->fAttach(it->fJson, shapeCtx->fCtx, std::move(drawGeos));
            }

            // If we still have multiple geos, reduce using 'merge'.
            auto geo = drawGeos.size() > 1
                ? sksg::Merge::Make(std::move(drawGeos), sksg::Merge::Mode::kMerge)
                : drawGeos[0];

            SkASSERT(geo);
            draws.push_back(sksg::Draw::Make(std::move(geo), std::move(paint)));
            shapeCtx->fCommittedAnimators = shapeCtx->fCtx->fAnimators.size();
        } break;
        default:
            break;
        }
    }

    // By now we should have popped all local geometry effects.
    SkASSERT(shapeCtx->fGeometryEffectStack->size() == initialGeometryEffects);

    // Push transformed local geometries to parent list, for subsequent paints.
    for (const auto& geo : geos) {
        shapeCtx->fGeometryStack->push_back(shape_matrix
            ? sksg::GeometryTransform::Make(std::move(geo), shape_matrix)
            : std::move(geo));
    }

    // Emit local draws reversed (bottom->top, per spec).
    for (auto it = draws.rbegin(); it != draws.rend(); ++it) {
        shape_group->addChild(std::move(*it));
    }

    return draws.empty() ? nullptr : shape_wrapper;
}

sk_sp<sksg::RenderNode> AttachNestedAnimation(const char* path, AttachContext* ctx) {
    class SkottieSGAdapter final : public sksg::RenderNode {
    public:
        explicit SkottieSGAdapter(sk_sp<Animation> animation)
            : fAnimation(std::move(animation)) {
            SkASSERT(fAnimation);
        }

    protected:
        SkRect onRevalidate(sksg::InvalidationController*, const SkMatrix&) override {
            return SkRect::MakeSize(fAnimation->size());
        }

        void onRender(SkCanvas* canvas) const override {
            fAnimation->render(canvas);
        }

    private:
        const sk_sp<Animation> fAnimation;
    };

    class SkottieAnimatorAdapter final : public sksg::Animator {
    public:
        SkottieAnimatorAdapter(sk_sp<Animation> animation, float frameRate)
            : fAnimation(std::move(animation))
            , fFrameRate(frameRate) {
            SkASSERT(fAnimation);
            SkASSERT(fFrameRate > 0);
        }

    protected:
        void onTick(float t) {
            // map back from frame # to ms.
            const auto t_ms = t * 1000 / fFrameRate;
            fAnimation->animationTick(t_ms);
        }

    private:
        const sk_sp<Animation> fAnimation;
        const float            fFrameRate;
    };

    const auto resStream  = ctx->fResources.openStream(path);
    if (!resStream || !resStream->hasLength()) {
        LOG("!! Could not open: %s\n", path);
        return nullptr;
    }

    auto animation = Animation::Make(resStream.get(), ctx->fResources);
    if (!animation) {
        LOG("!! Could not load nested animation: %s\n", path);
        return nullptr;
    }

    ctx->fAnimators.push_back(skstd::make_unique<SkottieAnimatorAdapter>(animation,
                                                                         ctx->fFrameRate));

    return sk_make_sp<SkottieSGAdapter>(std::move(animation));
}

sk_sp<sksg::RenderNode> AttachCompLayer(const Json::Value& jlayer, AttachContext* ctx,
                                        float* time_bias, float* time_scale) {
    SkASSERT(jlayer.isObject());

    SkString refId;
    if (!Parse(jlayer["refId"], &refId) || refId.isEmpty()) {
        LOG("!! Comp layer missing refId\n");
        return nullptr;
    }

    const auto start_time = ParseDefault(jlayer["st"], 0.0f),
             stretch_time = ParseDefault(jlayer["sr"], 1.0f);

    *time_bias = -start_time;
    *time_scale = 1 / stretch_time;
    if (SkScalarIsNaN(*time_scale)) {
        *time_scale = 1;
    }

    if (refId.startsWith("$")) {
        return AttachNestedAnimation(refId.c_str() + 1, ctx);
    }

    const auto* comp = ctx->fAssets.find(refId);
    if (!comp) {
        LOG("!! Pre-comp not found: '%s'\n", refId.c_str());
        return nullptr;
    }

    // TODO: cycle detection
    return AttachComposition(**comp, ctx);
}

sk_sp<sksg::RenderNode> AttachSolidLayer(const Json::Value& jlayer, AttachContext*,
                                         float*, float*) {
    SkASSERT(jlayer.isObject());

    const auto size = SkSize::Make(ParseDefault(jlayer["sw"], 0.0f),
                                   ParseDefault(jlayer["sh"], 0.0f));
    const auto hex = ParseDefault(jlayer["sc"], SkString());
    uint32_t c;
    if (size.isEmpty() ||
        !hex.startsWith("#") ||
        !SkParse::FindHex(hex.c_str() + 1, &c)) {
        LogFail(jlayer, "Could not parse solid layer");
        return nullptr;
    }

    const SkColor color = 0xff000000 | c;

    return sksg::Draw::Make(sksg::Rect::Make(SkRect::MakeSize(size)),
                            sksg::Color::Make(color));
}

sk_sp<sksg::RenderNode> AttachImageAsset(const Json::Value& jimage, AttachContext* ctx) {
    SkASSERT(jimage.isObject());

    const auto name = ParseDefault(jimage["p"], SkString()),
               path = ParseDefault(jimage["u"], SkString());
    if (name.isEmpty())
        return nullptr;

    // TODO: plumb resource paths explicitly to ResourceProvider?
    const auto resName    = path.isEmpty() ? name : SkOSPath::Join(path.c_str(), name.c_str());
    const auto resStream  = ctx->fResources.openStream(resName.c_str());
    if (!resStream || !resStream->hasLength()) {
        LOG("!! Could not load image resource: %s\n", resName.c_str());
        return nullptr;
    }

    // TODO: non-intrisic image sizing
    return sksg::Image::Make(
        SkImage::MakeFromEncoded(SkData::MakeFromStream(resStream.get(), resStream->getLength())));
}

sk_sp<sksg::RenderNode> AttachImageLayer(const Json::Value& layer, AttachContext* ctx,
                                         float*, float*) {
    SkASSERT(layer.isObject());

    SkString refId;
    if (!Parse(layer["refId"], &refId) || refId.isEmpty()) {
        LOG("!! Image layer missing refId\n");
        return nullptr;
    }

    const auto* jimage = ctx->fAssets.find(refId);
    if (!jimage) {
        LOG("!! Image asset not found: '%s'\n", refId.c_str());
        return nullptr;
    }

    return AttachImageAsset(**jimage, ctx);
}

sk_sp<sksg::RenderNode> AttachNullLayer(const Json::Value& layer, AttachContext*, float*, float*) {
    SkASSERT(layer.isObject());

    // Null layers are used solely to drive dependent transforms,
    // but we use free-floating sksg::Matrices for that purpose.
    return nullptr;
}

sk_sp<sksg::RenderNode> AttachShapeLayer(const Json::Value& layer, AttachContext* ctx,
                                         float*, float*) {
    SkASSERT(layer.isObject());

    std::vector<sk_sp<sksg::GeometryNode>> geometryStack;
    std::vector<GeometryEffectRec> geometryEffectStack;
    AttachShapeContext shapeCtx(ctx, &geometryStack, &geometryEffectStack, ctx->fAnimators.size());
    auto shapeNode = AttachShape(layer["shapes"], &shapeCtx);

    // Trim uncommitted animators: AttachShape consumes effects on the fly, and greedily attaches
    // geometries => at the end, we can end up with unused geometries, which are nevertheless alive
    // due to attached animators.  To avoid this, we track committed animators and discard the
    // orphans here.
    SkASSERT(shapeCtx.fCommittedAnimators <= ctx->fAnimators.size());
    ctx->fAnimators.resize(shapeCtx.fCommittedAnimators);

    return shapeNode;
}

sk_sp<sksg::RenderNode> AttachTextLayer(const Json::Value& layer, AttachContext*, float*, float*) {
    SkASSERT(layer.isObject());

    LOG("?? Text layer stub\n");
    return nullptr;
}

struct AttachLayerContext {
    AttachLayerContext(const Json::Value& jlayers, AttachContext* ctx)
        : fLayerList(jlayers), fCtx(ctx) {}

    const Json::Value&                   fLayerList;
    AttachContext*                       fCtx;
    SkTHashMap<int, sk_sp<sksg::Matrix>> fLayerMatrixMap;
    sk_sp<sksg::RenderNode>              fCurrentMatte;

    sk_sp<sksg::Matrix> AttachParentLayerMatrix(const Json::Value& jlayer) {
        SkASSERT(jlayer.isObject());
        SkASSERT(fLayerList.isArray());

        const auto parent_index = ParseDefault(jlayer["parent"], -1);
        if (parent_index < 0)
            return nullptr;

        if (auto* m = fLayerMatrixMap.find(parent_index))
            return *m;

        for (const auto& l : fLayerList) {
            if (!l.isObject()) {
                continue;
            }

            if (ParseDefault(l["ind"], -1) == parent_index) {
                return this->AttachLayerMatrix(l);
            }
        }

        return nullptr;
    }

    sk_sp<sksg::Matrix> AttachLayerMatrix(const Json::Value& jlayer) {
        SkASSERT(jlayer.isObject());

        const auto layer_index = ParseDefault(jlayer["ind"], -1);
        if (layer_index < 0)
            return nullptr;

        if (auto* m = fLayerMatrixMap.find(layer_index))
            return *m;

        // Add a stub entry to break recursion cycles.
        fLayerMatrixMap.set(layer_index, nullptr);

        auto parent_matrix = this->AttachParentLayerMatrix(jlayer);

        return *fLayerMatrixMap.set(layer_index,
                                    AttachMatrix(jlayer["ks"],
                                                 fCtx,
                                                 this->AttachParentLayerMatrix(jlayer)));
    }
};

SkBlendMode MaskBlendMode(char mode) {
    switch (mode) {
    case 'a': return SkBlendMode::kSrcOver;    // Additive
    case 's': return SkBlendMode::kExclusion;  // Subtract
    case 'i': return SkBlendMode::kDstIn;      // Intersect
    case 'l': return SkBlendMode::kLighten;    // Lighten
    case 'd': return SkBlendMode::kDarken;     // Darken
    case 'f': return SkBlendMode::kDifference; // Difference
    default: break;
    }

    return SkBlendMode::kSrcOver;
}

sk_sp<sksg::RenderNode> AttachMask(const Json::Value& jmask,
                                   AttachContext* ctx,
                                   sk_sp<sksg::RenderNode> childNode) {
    if (!jmask.isArray())
        return childNode;

    struct MaskRecord {
        sk_sp<sksg::Path>  mask_path;
        sk_sp<sksg::Color> mask_paint;
    };

    SkSTArray<4, MaskRecord, true> mask_stack;

    bool opaque_mask = true;

    for (const auto& m : jmask) {
        if (!m.isObject())
            continue;

        auto mask_path = AttachPath(m["pt"], ctx);
        if (!mask_path) {
            LogFail(m, "Could not parse mask path");
            continue;
        }

        mask_path->setFillType(ParseDefault(m["inv"], false)
            ? SkPath::kInverseWinding_FillType
            : SkPath::kWinding_FillType);

        SkString mode;
        if (!Parse(m["mode"], &mode) ||
            mode.size() != 1 ||
            !strcmp(mode.c_str(), "n")) { // "None" masks have no effect.
            continue;
        }

        auto mask_paint = sksg::Color::Make(SK_ColorBLACK);
        mask_paint->setAntiAlias(true);
        mask_paint->setBlendMode(MaskBlendMode(mode.c_str()[0]));

        const auto animator_count = ctx->fAnimators.size();
        BindProperty<ScalarValue>(m["o"], &ctx->fAnimators,
            [mask_paint](const ScalarValue& o) { mask_paint->setOpacity(o * 0.01f); });

        opaque_mask &= (animator_count == ctx->fAnimators.size() && mask_paint->getOpacity() >= 1);

        mask_stack.push_back({mask_path, mask_paint});
    }

    if (mask_stack.empty())
        return childNode;

    if (mask_stack.count() == 1 && opaque_mask) {
        // Single opaque mask => clip path.
        return sksg::ClipEffect::Make(std::move(childNode),
                                      std::move(mask_stack.front().mask_path),
                                      true);
    }

    auto mask_group = sksg::Group::Make();
    for (const auto& rec : mask_stack) {
        mask_group->addChild(sksg::Draw::Make(std::move(rec.mask_path),
                                              std::move(rec.mask_paint)));

    }

    return sksg::MaskEffect::Make(std::move(childNode), std::move(mask_group));
}

sk_sp<sksg::RenderNode> AttachLayer(const Json::Value& jlayer,
                                    AttachLayerContext* layerCtx) {
    if (!jlayer.isObject())
        return nullptr;

    using LayerAttacher = sk_sp<sksg::RenderNode> (*)(const Json::Value&, AttachContext*,
                                                      float* time_bias, float* time_scale);
    static constexpr LayerAttacher gLayerAttachers[] = {
        AttachCompLayer,  // 'ty': 0
        AttachSolidLayer, // 'ty': 1
        AttachImageLayer, // 'ty': 2
        AttachNullLayer,  // 'ty': 3
        AttachShapeLayer, // 'ty': 4
        AttachTextLayer,  // 'ty': 5
    };

    int type = ParseDefault(jlayer["ty"], -1);
    if (type < 0 || type >= SkTo<int>(SK_ARRAY_COUNT(gLayerAttachers))) {
        return nullptr;
    }

    sksg::AnimatorList layer_animators;
    AttachContext local_ctx = { layerCtx->fCtx->fResources,
                                layerCtx->fCtx->fAssets,
                                layerCtx->fCtx->fFrameRate,
                                layer_animators};

    // Layer attachers may adjust these.
    float time_bias  = 0,
          time_scale = 1;

    // Layer content.
    auto layer = gLayerAttachers[type](jlayer, &local_ctx, &time_bias, &time_scale);

    // Clip layers with explicit dimensions.
    float w, h;
    if (Parse(jlayer["w"], &w) && Parse(jlayer["h"], &h)) {
        layer = sksg::ClipEffect::Make(std::move(layer),
                                       sksg::Rect::Make(SkRect::MakeWH(w, h)),
                                       true);
    }

    // Optional layer mask.
    layer = AttachMask(jlayer["masksProperties"], &local_ctx, std::move(layer));

    // Optional layer transform.
    if (auto layerMatrix = layerCtx->AttachLayerMatrix(jlayer)) {
        layer = sksg::Transform::Make(std::move(layer), std::move(layerMatrix));
    }

    // Optional layer opacity.
    layer = AttachOpacity(jlayer["ks"], &local_ctx, std::move(layer));

    class LayerController final : public sksg::GroupAnimator {
    public:
        LayerController(sksg::AnimatorList&& layer_animators,
                        sk_sp<sksg::OpacityEffect> controlNode,
                        float in, float out,
                        float time_bias, float time_scale)
            : INHERITED(std::move(layer_animators))
            , fControlNode(std::move(controlNode))
            , fIn(in)
            , fOut(out)
            , fTimeBias(time_bias)
            , fTimeScale(time_scale) {}

        void onTick(float t) override {
            const auto active = (t >= fIn && t <= fOut);

            // Keep the layer fully transparent except for its [in..out] lifespan.
            // (note: opacity == 0 disables rendering, while opacity == 1 is a noop)
            fControlNode->setOpacity(active ? 1 : 0);

            // Dispatch ticks only while active.
            if (active)
                this->INHERITED::onTick((t + fTimeBias) * fTimeScale);
        }

    private:
        const sk_sp<sksg::OpacityEffect> fControlNode;
        const float                      fIn,
                                         fOut,
                                         fTimeBias,
                                         fTimeScale;

        using INHERITED = sksg::GroupAnimator;
    };

    auto controller_node = sksg::OpacityEffect::Make(std::move(layer));
    const auto        in = ParseDefault(jlayer["ip"], 0.0f),
                     out = ParseDefault(jlayer["op"], in);

    if (!jlayer["tm"].isNull()) {
        LogFail(jlayer["tm"], "Unsupported time remapping");
    }

    if (in >= out || !controller_node)
        return nullptr;

    layerCtx->fCtx->fAnimators.push_back(
        skstd::make_unique<LayerController>(std::move(layer_animators),
                                            controller_node,
                                            in,
                                            out,
                                            time_bias,
                                            time_scale));

    if (ParseDefault(jlayer["td"], false)) {
        // This layer is a matte.  We apply it as a mask to the next layer.
        layerCtx->fCurrentMatte = std::move(controller_node);
        return nullptr;
    }

    if (layerCtx->fCurrentMatte) {
        // There is a pending matte. Apply and reset.
        static constexpr sksg::MaskEffect::Mode gMaskModes[] = {
            sksg::MaskEffect::Mode::kNormal, // tt: 1
            sksg::MaskEffect::Mode::kInvert, // tt: 2
        };
        const auto matteType = ParseDefault(jlayer["tt"], 1) - 1;

        if (matteType >= 0 && matteType < SkTo<int>(SK_ARRAY_COUNT(gMaskModes))) {
            return sksg::MaskEffect::Make(std::move(controller_node),
                                          std::move(layerCtx->fCurrentMatte),
                                          gMaskModes[matteType]);
        }
        layerCtx->fCurrentMatte.reset();
    }

    return std::move(controller_node);
}

sk_sp<sksg::RenderNode> AttachComposition(const Json::Value& comp, AttachContext* ctx) {
    if (!comp.isObject())
        return nullptr;

    const auto& jlayers = comp["layers"];
    if (!jlayers.isArray())
        return nullptr;

    SkSTArray<16, sk_sp<sksg::RenderNode>, true> layers;
    AttachLayerContext                           layerCtx(jlayers, ctx);

    for (const auto& l : jlayers) {
        if (auto layer_fragment = AttachLayer(l, &layerCtx)) {
            layers.push_back(std::move(layer_fragment));
        }
    }

    if (layers.empty()) {
        return nullptr;
    }

    // Layers are painted in bottom->top order.
    auto comp_group = sksg::Group::Make();
    for (int i = layers.count() - 1; i >= 0; --i) {
        comp_group->addChild(std::move(layers[i]));
    }

    return std::move(comp_group);
}

} // namespace

sk_sp<Animation> Animation::Make(SkStream* stream, const ResourceProvider& res, Stats* stats) {
    Stats stats_storage;
    if (!stats)
        stats = &stats_storage;
    memset(stats, 0, sizeof(struct Stats));

    if (!stream->hasLength()) {
        // TODO: handle explicit buffering?
        LOG("!! cannot parse streaming content\n");
        return nullptr;
    }

    const auto t0 = SkTime::GetMSecs();

    Json::Value json;
    {
        auto data = SkData::MakeFromStream(stream, stream->getLength());
        if (!data) {
            LOG("!! could not read stream\n");
            return nullptr;
        }
        stats->fJsonSize = data->size();

        Json::Reader reader;

        auto dataStart = static_cast<const char*>(data->data());
        if (!reader.parse(dataStart, dataStart + data->size(), json, false) || !json.isObject()) {
            LOG("!! failed to parse json: %s\n", reader.getFormattedErrorMessages().c_str());
            return nullptr;
        }
    }

    const auto t1 = SkTime::GetMSecs();
    stats->fJsonParseTimeMS = t1 - t0;

    const auto version = ParseDefault(json["v"], SkString());
    const auto size    = SkSize::Make(ParseDefault(json["w"], 0.0f),
                                      ParseDefault(json["h"], 0.0f));
    const auto fps     = ParseDefault(json["fr"], -1.0f);

    if (size.isEmpty() || version.isEmpty() || fps <= 0) {
        LOG("!! invalid animation params (version: %s, size: [%f %f], frame rate: %f)",
            version.c_str(), size.width(), size.height(), fps);
        return nullptr;
    }

    const auto anim =
        sk_sp<Animation>(new Animation(res, std::move(version), size, fps, json, stats));
    const auto t2 = SkTime::GetMSecs();
    stats->fSceneParseTimeMS = t2 - t1;
    stats->fTotalLoadTimeMS  = t2 - t0;

    return anim;
}

sk_sp<Animation> Animation::MakeFromFile(const char path[], const ResourceProvider* res,
                                         Stats* stats) {
    class DirectoryResourceProvider final : public ResourceProvider {
    public:
        explicit DirectoryResourceProvider(SkString dir) : fDir(std::move(dir)) {}

        std::unique_ptr<SkStream> openStream(const char resource[]) const override {
            const auto resPath = SkOSPath::Join(fDir.c_str(), resource);
            return SkStream::MakeFromFile(resPath.c_str());
        }

    private:
        const SkString fDir;
    };

    const auto jsonStream =  SkStream::MakeFromFile(path);
    if (!jsonStream)
        return nullptr;

    std::unique_ptr<ResourceProvider> defaultProvider;
    if (!res) {
        defaultProvider = skstd::make_unique<DirectoryResourceProvider>(SkOSPath::Dirname(path));
    }

    return Make(jsonStream.get(), res ? *res : *defaultProvider, stats);
}

Animation::Animation(const ResourceProvider& resources,
                     SkString version, const SkSize& size, SkScalar fps, const Json::Value& json,
                     Stats* stats)
    : fVersion(std::move(version))
    , fSize(size)
    , fFrameRate(fps)
    , fInPoint(ParseDefault(json["ip"], 0.0f))
    , fOutPoint(SkTMax(ParseDefault(json["op"], SK_ScalarMax), fInPoint)) {

    AssetMap assets;
    for (const auto& asset : json["assets"]) {
        if (!asset.isObject()) {
            continue;
        }

        assets.set(ParseDefault(asset["id"], SkString()), &asset);
    }

    sksg::AnimatorList animators;
    AttachContext ctx = { resources, assets, fFrameRate, animators };
    auto root = AttachComposition(json, &ctx);

    stats->fAnimatorCount = animators.size();

    fScene = sksg::Scene::Make(std::move(root), std::move(animators));

    // In case the client calls render before the first tick.
    this->animationTick(0);
}

Animation::~Animation() = default;

void Animation::setShowInval(bool show) {
    if (fScene) {
        fScene->setShowInval(show);
    }
}

void Animation::render(SkCanvas* canvas, const SkRect* dstR) const {
    if (!fScene)
        return;

    SkAutoCanvasRestore restore(canvas, true);
    const SkRect srcR = SkRect::MakeSize(this->size());
    if (dstR) {
        canvas->concat(SkMatrix::MakeRectToRect(srcR, *dstR, SkMatrix::kCenter_ScaleToFit));
    }
    canvas->clipRect(srcR);
    fScene->render(canvas);
}

void Animation::animationTick(SkMSec ms) {
    if (!fScene)
        return;

    // 't' in the BM model really means 'frame #'
    auto t = static_cast<float>(ms) * fFrameRate / 1000;

    t = fInPoint + std::fmod(t, fOutPoint - fInPoint);

    fScene->animate(t);
}

} // namespace skottie
