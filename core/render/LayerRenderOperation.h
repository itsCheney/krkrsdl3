#pragma once
#include <cstdint>

// Common software RenderManager semantics, independent of Emote blend modes.
enum class TVPLayerOperationKind : uint32_t
{
    Unsupported, Copy, CopyColor, CopyMask, CopyOpaque, Fill, FillColor,
    FillMask, Alpha, ConstAlpha, ColorMap
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
struct TVPLayerRenderStats
{
    uint64_t gpuOperations = 0, cpuFallbacks = 0;
    uint64_t uploadedBytes = 0, readbackBytes = 0;
    uint64_t gpuResidentBytes = 0, cpuCacheBytes = 0;
    uint64_t pinnedCPUTextures = 0;
};
