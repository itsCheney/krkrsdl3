#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>

// Common software RenderManager semantics, independent of Emote blend modes.
#define TVP_LAYER_OPERATION_ENUM_ROW(name, id, ...) name = id,
enum class TVPLayerOperationKind : uint32_t
{
#define TVP_LAYER_OPERATION TVP_LAYER_OPERATION_ENUM_ROW
#define TVP_LAYER_OPERATION_COUNT(count) Count = count
#include "LayerOperationDefinitions.def"
#undef TVP_LAYER_OPERATION_COUNT
#undef TVP_LAYER_OPERATION
};
inline constexpr std::size_t TVP_LAYER_OPERATION_COUNT =
    static_cast<std::size_t>(TVPLayerOperationKind::Count);
// These names are also injected into runtime MSL and used by scalar extraction.
#define TVP_LAYER_OPERATION(name, id, ...) inline constexpr int TVP_LAYER_KIND_##name = id;
#define TVP_LAYER_OPERATION_COUNT(count)
#include "LayerOperationDefinitions.def"
#undef TVP_LAYER_OPERATION_COUNT
#undef TVP_LAYER_OPERATION
enum TVPLayerOperationFlags : uint32_t
{
    TVP_LAYER_HOLD_ALPHA = 1,
    TVP_LAYER_DEST_ALPHA = 2,
    TVP_LAYER_DEST_PREMULTIPLIED = 4,
    TVP_LAYER_FULL_OPACITY_BRANCH = 8,
};
// Immutable parameter bytes follow tTVPGLGammaAdjustTempData's B/G/R layout.
// Operations retain the snapshot, never a caller's gammaAdjustData pointer.
struct TVPLayerGammaLUT
{
    uint64_t version = 0;
    std::array<uint8_t,768> bytes{};
};
struct TVPLayerParameterUploadStats
{
    uint64_t gammaLUTUploads = 0;
    uint64_t gammaLUTUploadedBytes = 0;
    uint64_t psTableUploads = 0;
    uint64_t psTableUploadedBytes = 0;
};
struct TVPLayerOperation
{
    TVPLayerOperationKind kind = TVPLayerOperationKind::Unsupported;
    int opacity = 255;
    uint32_t color = 0;
    uint32_t flags = 0;
    // For BoxBlur these carry the software kernel width and height.
    int phase = 0;
    int vague = 0;
    std::shared_ptr<const TVPLayerGammaLUT> gammaLUT;
};
enum class TVPLayerTextureFormat { RGBA8, R8 };
enum class TVPLayerReferenceRule { Ignored, UsedWhenNoInput };
enum class TVPLayerAlphaRule { Writes, Preserves, PlainHDA, SourceAlpha };
enum class TVPLayerAliasRule {
    Unsupported, Ignored, Snapshot, SamePixelOnly, ReadAllBeforeWrite
};
enum TVPLayerParameterResources : uint32_t {
    TVP_LAYER_RESOURCE_ALPHA_TABLES = 1,
    // Gamma has an owned per-call LUT; image upload counters exclude it.
    TVP_LAYER_RESOURCE_GAMMA_LUT = 2,
    TVP_LAYER_RESOURCE_PS_TABLES = 4
};
enum TVPLayerGeometry : uint32_t {
    TVP_LAYER_GEOMETRY_RECT = 1,
    // Copy/flags=0 only, subject to the existing affine Prepare/clip checks.
    TVP_LAYER_GEOMETRY_AFFINE_COPY_SUBSET = 2,
    // Prepared quad sampling followed by the ordinary pixel blend. Geometry,
    // rectangle overlap and resource checks remain at the execution boundary.
    TVP_LAYER_GEOMETRY_AFFINE_BLEND_SUBSET = 4
};
struct TVPLayerOperationTraits {
    TVPLayerOperationKind kind;
    const char* name;
    uint8_t logicalInputCountMask;
    uint8_t backendInputCount;
    TVPLayerTextureFormat targetFormat;
    TVPLayerTextureFormat sourceFormats[3];
    TVPLayerReferenceRule referenceRule;
    bool readsTarget;
    TVPLayerAlphaRule alphaRule;
    TVPLayerAliasRule aliasRule;
    uint32_t parameterResources;
    uint32_t geometries;
};
#define TVP_LAYER_OPERATION_TRAIT_ROW(name, id, mask, inputs, f0, f1, f2, reference, reads, alpha, alias, resources, geometry) \
    {static_cast<TVPLayerOperationKind>(id), #name, mask, inputs, TVPLayerTextureFormat::RGBA8, \
     {TVPLayerTextureFormat::f0, TVPLayerTextureFormat::f1, TVPLayerTextureFormat::f2}, \
     TVPLayerReferenceRule::reference, reads, TVPLayerAlphaRule::alpha, TVPLayerAliasRule::alias, resources, geometry},
inline constexpr TVPLayerOperationTraits TVP_LAYER_OPERATION_TRAITS[] = {
#define TVP_LAYER_OPERATION TVP_LAYER_OPERATION_TRAIT_ROW
#define TVP_LAYER_OPERATION_COUNT(count)
#include "LayerOperationDefinitions.def"
#undef TVP_LAYER_OPERATION_COUNT
#undef TVP_LAYER_OPERATION
};
template<std::size_t N>
constexpr bool TVPLayerOperationDefinitionsValid(const TVPLayerOperationTraits (&traits)[N]) {
    for (std::size_t i = 0; i < N; ++i)
        if (static_cast<std::size_t>(traits[i].kind) != i) return false;
    return true;
}
constexpr bool TVPLayerOperationDefinitionsValid() {
    return sizeof(TVP_LAYER_OPERATION_TRAITS) / sizeof(TVP_LAYER_OPERATION_TRAITS[0]) == TVP_LAYER_OPERATION_COUNT &&
           TVPLayerOperationDefinitionsValid(TVP_LAYER_OPERATION_TRAITS);
}
static_assert(TVPLayerOperationDefinitionsValid(), "Layer IDs/traits/Count must agree");
template<std::size_t N>
inline constexpr const TVPLayerOperationTraits* TVPFindLayerOperationTraits(
        const TVPLayerOperationTraits (&traits)[N], uint32_t id) {
    return id > 0 && id < N ? &traits[id] : nullptr;
}
inline constexpr const TVPLayerOperationTraits* TVPGetLayerOperationTraits(TVPLayerOperationKind kind) {
    const auto id = static_cast<uint32_t>(kind);
    return TVPFindLayerOperationTraits(TVP_LAYER_OPERATION_TRAITS, id);
}
inline constexpr bool TVPLayerOperationNeedsSource(TVPLayerOperationKind kind) {
    const auto* traits = TVPGetLayerOperationTraits(kind);
    return traits && traits->backendInputCount != 0;
}
inline constexpr bool TVPLayerOperationReadsTarget(TVPLayerOperationKind kind) {
    const auto* traits = TVPGetLayerOperationTraits(kind);
    return traits && traits->readsTarget;
}
inline constexpr bool TVPLayerOperationSupportsAffine(const TVPLayerOperation& op) {
    const auto* traits = TVPGetLayerOperationTraits(op.kind);
    if (!traits) return false;
    if (traits->geometries & TVP_LAYER_GEOMETRY_AFFINE_COPY_SUBSET)
        return op.flags == 0;
    constexpr uint32_t flags = TVP_LAYER_HOLD_ALPHA | TVP_LAYER_DEST_ALPHA |
        TVP_LAYER_DEST_PREMULTIPLIED | TVP_LAYER_FULL_OPACITY_BRANCH;
    return (traits->geometries & TVP_LAYER_GEOMETRY_AFFINE_BLEND_SUBSET) &&
        !(op.flags & ~flags) && op.opacity >= 0 && op.opacity <= 255;
}
// P1A's software blend/ApplySelf wrappers do not define mirrored source
// rectangles. Preserve earlier kinds' routing and keep new domains explicit.
inline constexpr bool TVPLayerOperationRequiresForwardSource(TVPLayerOperationKind kind) {
    switch(kind) {
        case TVPLayerOperationKind::Sub: case TVPLayerOperationKind::Mul:
        case TVPLayerOperationKind::ColorDodge: case TVPLayerOperationKind::Darken:
        case TVPLayerOperationKind::Lighten: case TVPLayerOperationKind::Screen:
        case TVPLayerOperationKind::RemoveOpacity: case TVPLayerOperationKind::AdditiveAlphaToAlpha:
        case TVPLayerOperationKind::AlphaSD: return true;
        case TVPLayerOperationKind::PsAlpha: case TVPLayerOperationKind::PsAdd:
        case TVPLayerOperationKind::PsSub: case TVPLayerOperationKind::PsSoftLight:
        case TVPLayerOperationKind::PsColorDodge: case TVPLayerOperationKind::PsColorBurn:
        case TVPLayerOperationKind::PsLighten: case TVPLayerOperationKind::PsDarken:
        case TVPLayerOperationKind::PsDiff: case TVPLayerOperationKind::PsDiff5:
        case TVPLayerOperationKind::PsExclusion: return true;
        default: return false;
    }
}
inline constexpr bool TVPLayerOperationPreservesAlpha(const TVPLayerOperation& op, bool sourceIsTarget) {
    const auto* traits = TVPGetLayerOperationTraits(op.kind);
    if (!traits) return false;
    switch (traits->alphaRule) {
        case TVPLayerAlphaRule::Preserves: return true;
        case TVPLayerAlphaRule::SourceAlpha: return sourceIsTarget;
        case TVPLayerAlphaRule::PlainHDA:
            return (op.flags & TVP_LAYER_HOLD_ALPHA) &&
                   !(op.flags & (TVP_LAYER_DEST_ALPHA | TVP_LAYER_DEST_PREMULTIPLIED));
        default: return false;
    }
}
inline constexpr bool TVPLayerOperationNeedsAlphaTables(const TVPLayerOperation& op) {
    const auto* traits = TVPGetLayerOperationTraits(op.kind);
    return traits && (traits->parameterResources & TVP_LAYER_RESOURCE_ALPHA_TABLES) &&
           (op.flags & TVP_LAYER_DEST_ALPHA);
}
struct TVPLayerRect
{
    int left = 0, top = 0, right = 0, bottom = 0;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};
// Prepared software-compatible affine inverse map. Coordinates are relative
// to clip/sourceCrop; no render-method pointers cross the backend boundary.
struct TVPLayerAffineCopy {
    TVPLayerRect clip, sourceCrop;
    double inverse[6] = {}; // source x/y = a*x + b*y + c, destination centers
};
// Why a GPU->CPU readback happened. Readbacks are synchronous and dominate
// main-thread time, so attribution matters more than the total: the same byte
// count means very different things for a per-frame present than for a one-off
// script query.
enum class TVPLayerReadbackSource
{
    // Explicit read lock, e.g. layer hit testing sampling a single pixel.
    Lock = 0,
    // A software operator ran because the GPU path could not take it.
    Fallback,
    // Raw pixel pointer exposed to a script or plugin.
    Persistent,
    // Scanline access, GetPoint, or a partial Update needing existing pixels.
    Pixels,
    // Session teardown moving a texture back to CPU ownership.
    Detach,
    // Single-pixel query satisfied by a tiny backend region readback.
    Point,
    Count
};
// Which operand first forced a GPU texture into CPU memory during a software
// fallback. Attribution is only recorded when an actual readback happens.
enum class TVPLayerFallbackReadbackRole
{
    Target = 0,
    Source,
    Reference,
    Count
};
// Why an operation could not stay on the Metal Layer path. Rect operations
// record one reason before entering software; unsupported triangle/perspective
// calls are tracked explicitly after their specialized GPU path is considered.
enum class TVPLayerGPURejectReason
{
    TargetUnavailable = 0,
    TargetCPUResident,
    MultipleInputs,
    UnsupportedMethod,
    UnsupportedStretch,
    InvalidOpacity,
    SourceUnavailable,
    SourceFormat,
    InvalidGeometry,
    UnsupportedKind,
    AlphaTables,
    BackendFailure,
    Triangles,
    Perspective,
    PsTables,
    // Rectangular affine shortcuts can retain software scanline ordering.
    AffineAlias,
    Count
};
struct TVPLayerRenderStats
{
    uint64_t gpuOperations = 0, cpuFallbacks = 0;
    uint64_t uploadedBytes = 0, readbackBytes = 0;
    uint64_t gpuResidentBytes = 0, cpuCacheBytes = 0;
    uint64_t pinnedCPUTextures = 0;
    // Indexed by TVPLayerReadbackSource; sums to readbackBytes.
    uint64_t readbackBytesBySource[static_cast<int>(TVPLayerReadbackSource::Count)] = {};
    uint64_t readbackCountBySource[static_cast<int>(TVPLayerReadbackSource::Count)] = {};
    // Fallback-only attribution and GPU reject reasons. These are diagnostic
    // counters and do not affect render-path selection.
    uint64_t fallbackReadbackBytesByRole[static_cast<int>(TVPLayerFallbackReadbackRole::Count)] = {};
    uint64_t fallbackReadbackCountByRole[static_cast<int>(TVPLayerFallbackReadbackRole::Count)] = {};
    uint64_t gpuRejectCountByReason[static_cast<int>(TVPLayerGPURejectReason::Count)] = {};
    uint64_t pointCacheHits = 0;
    uint64_t pointCacheMisses = 0;
    uint64_t gammaLUTUploads = 0;
    uint64_t gammaLUTUploadedBytes = 0;
    uint64_t psTableUploads = 0;
    uint64_t psTableUploadedBytes = 0;
};
