#pragma once
#include <cstdint>

// Common software RenderManager semantics, independent of Emote blend modes.
enum class TVPLayerOperationKind : uint32_t
{
    Unsupported, Copy, CopyColor, CopyMask, CopyOpaque, Fill, FillColor,
    FillMask, Alpha, ConstAlpha, ColorMap, FillBlend, RemoveConstOpacity,
    ConstAlphaSD
};
enum TVPLayerOperationFlags : uint32_t
{
    TVP_LAYER_HOLD_ALPHA = 1,
    TVP_LAYER_DEST_ALPHA = 2,
    TVP_LAYER_DEST_PREMULTIPLIED = 4,
    TVP_LAYER_FULL_OPACITY_BRANCH = 8,
};
struct TVPLayerOperation
{
    TVPLayerOperationKind kind = TVPLayerOperationKind::Unsupported;
    int opacity = 255;
    uint32_t color = 0;
    uint32_t flags = 0;
};
struct TVPLayerRect
{
    int left = 0, top = 0, right = 0, bottom = 0;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};
enum class TVPLayerTextureFormat { RGBA8, R8 };
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
// record one reason before entering software; triangle/perspective calls are
// tracked explicitly because they do not attempt the rect GPU path.
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
};
