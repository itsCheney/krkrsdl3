#pragma once
#include "../LayerRenderOperation.h"
#include <array>
#include <string>

namespace krkrsdl3::ordinary_layer {
enum class Pipeline : unsigned { Generic, Copy, Fill, CopyColor, AlphaHDA, Count };
inline constexpr unsigned PipelineCount = unsigned(Pipeline::Count);
// Keep the logical operation's target-read/COW contract unchanged. These are
// shader variants only; all resource and geometry checks precede selection.
inline constexpr Pipeline Select(int kind, int opacity, uint32_t flags) {
    if(flags == 0) {
        if(kind == TVP_LAYER_KIND_Copy) return Pipeline::Copy;
        if(kind == TVP_LAYER_KIND_Fill) return Pipeline::Fill;
        if(kind == TVP_LAYER_KIND_CopyColor) return Pipeline::CopyColor;
    }
    if(kind == TVP_LAYER_KIND_Alpha && opacity == 255 &&
       flags == (TVP_LAYER_HOLD_ALPHA | TVP_LAYER_FULL_OPACITY_BRANCH))
        return Pipeline::AlphaHDA;
    return Pipeline::Generic;
}
inline constexpr const char* FunctionName(Pipeline pipeline) {
    switch(pipeline) {
        case Pipeline::Copy: return "ordinaryLayerCopyFragment";
        case Pipeline::Fill: return "ordinaryLayerFillFragment";
        case Pipeline::CopyColor: return "ordinaryLayerCopyColorFragment";
        case Pipeline::AlphaHDA: return "ordinaryLayerAlphaHDAFragment";
        default: return "ordinaryLayerFragment";
    }
}
inline constexpr bool WritesRGBOnly(Pipeline pipeline) {
    return pipeline == Pipeline::CopyColor || pipeline == Pipeline::AlphaHDA;
}
struct Counters {
    bool enabled = false;
    uint32_t availableMask = 0;
    // Actual encoded shader draws only, excluding load clears and omitted Fill.
    // Index order is Generic, Copy, Fill, CopyColor, AlphaHDA.
    std::array<uint64_t, PipelineCount> draws{}, pixels{};
    void Record(Pipeline pipeline, uint64_t coverage) {
        ++draws[unsigned(pipeline)]; pixels[unsigned(pipeline)] += coverage;
    }
    std::string Report() const {
        const char* names[]={"generic","copy","fill","copyColor","alphaHDA"};
        std::string result = "metal.layerPipelines version=1 final=1 enabled=";
        result += enabled ? "1" : "0";
        result += " availableMask=" + std::to_string(availableMask);
        for(unsigned i=0;i<PipelineCount;++i) {
            result += " " + std::string(names[i]) + "Draws=" + std::to_string(draws[i]);
            result += " " + std::string(names[i]) + "Pixels=" + std::to_string(pixels[i]);
        }
        return result;
    }
};
}
