#pragma once
#include "LayerRenderOperation.h"
#include <cstdint>
#include <vector>

// Ordered, already rasterized plutovg 1.3.3 blend calls. All pixels retain the
// original packed byte order and premultiplication; no renderer color conversion.
enum class TVPLayerSpanKind : uint32_t {
#define TVP_LAYER_SPAN(name,id) name=id,
#define TVP_LAYER_SPAN_COUNT(count) Count=count
#include "LayerSpanCompositeDefinitions.def"
#undef TVP_LAYER_SPAN_COUNT
#undef TVP_LAYER_SPAN
};
struct TVPLayerSpan {
    int32_t x=0,y=0;
    uint32_t length=0,coverage=0,kind=0,solid=0,sourceOffset=0,reserved=0;
};
static_assert(sizeof(TVPLayerSpan)==32,"span GPU ABI");
struct TVPLayerSpanCompositePacket {
    TVPLayerRect destination{};
    std::vector<TVPLayerSpan> spans;
    std::vector<uint32_t> sourcePixels,rowOffsets,rowEntries;
};
enum class TVPLayerSpanCompositeResult : uint32_t {
    Applied, Unsupported, Geometry, Resource, CPUAccess, ParameterBudget, BackendFailure
};
class iTVPTexture2D;
bool TVPHasMetalLayerSpanCompositionSupport();
// Read-only eligibility check; does not acquire pixels or touch a GPU handle.
TVPLayerSpanCompositeResult TVPCheckMetalLayerCPUOverwrite(iTVPTexture2D* target);
// False/rejected work never changes destination pixels. Post-commit exceptions
// propagate after invalidating texture caches, so callers must not replay them.
TVPLayerSpanCompositeResult TVPTryMetalLayerSpanComposite(
    const TVPLayerSpanCompositePacket&,iTVPTexture2D* target,bool* committed=nullptr);
