#pragma once
#include "LayerRenderOperation.h"
#include <cstdint>
#include <memory>
#include <vector>

enum class TVPLayerShrinkKind : uint32_t {
#define TVP_LAYER_SHRINK(name, value) name = value,
#define TVP_LAYER_SHRINK_COUNT(value) Count = value
#include "LayerShrinkDefinitions.def"
#undef TVP_LAYER_SHRINK
#undef TVP_LAYER_SHRINK_COUNT
};

// Pixel coordinates, not byte offsets. For Area the two edge samples are
// base-1 and base+step. A step of -1 intentionally repeats an endpoint.
struct TVPLayerShrinkAxis {
    int32_t base = 0, step = 0;
    uint64_t ta = 0, tc = 0, ba = 0, bc = 0, total = 0;
};
struct TVPLayerShrinkOperation {
    TVPLayerShrinkKind kind = TVPLayerShrinkKind::Unsupported;
    uint32_t avgBits = 32;
    TVPLayerRect destination{};
    uint32_t hu = 256, vu = 256;
    int32_t sourceTop = 0, sourceRows = 0;
    std::shared_ptr<const std::vector<TVPLayerShrinkAxis>> horizontal, vertical;
};
enum class TVPLayerShrinkResult : uint32_t {
    Applied, Unsupported, Geometry, Resource, CPUAccess, AliasDependency,
    Arithmetic, ParameterBudget, BackendFailure
};
class iTVPTexture2D;
bool TVPHasMetalLayerShrinkSupport();
TVPLayerShrinkResult TVPTryMetalLayerShrink(const TVPLayerShrinkOperation& operation,
    iTVPTexture2D* target, iTVPTexture2D* retainedSource);
