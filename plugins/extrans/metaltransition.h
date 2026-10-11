#pragma once

#include "LayerTransition.h"
#include "transhandler.h"
#include "LayerWorkDiagnostics.h"
#include <cstdio>

template<typename... Args>
inline void TVPSetTransitionMetadata(const char *format, Args... args) {
    if(!krkrsdl3::layer_work::enabled.load(std::memory_order_relaxed) || !krkrsdl3::layer_work::transitionScope) return;
    char metadata[512];
    std::snprintf(metadata, sizeof(metadata), format, args...);
    krkrsdl3::layer_work::SetTransitionMetadata(metadata);
}

inline bool TVPHasDivisibleMetalTransitionSupport(const tTVPDivisibleData *data) {
    if(TVPHasMetalLayerTransitionSupport()) return true;
    krkrsdl3::layer_work::RecordTransitionResult(false, "pipelineUnavailable",
        data && data->Width > 0 && data->Height > 0 ?
            static_cast<std::uint64_t>(data->Width) * data->Height : 0);
    return false;
}

inline TVPLayerTransitionResult TVPTryDivisibleMetalTransition(
        TVPLayerTransitionOperation &operation, tTVPDivisibleData *data) {
    if(!data || !data->Dest || !data->Src1 || !data->Src2)
    {
        krkrsdl3::layer_work::RecordTransitionResult(false, "invalidResource",
            data && data->Width > 0 && data->Height > 0 ?
                static_cast<std::uint64_t>(data->Width) * data->Height : 0);
        return TVPLayerTransitionResult::InvalidResource;
    }
    auto &p = operation.params;
    p.left = data->Left; p.top = data->Top;
    p.width = data->Width; p.height = data->Height;
    p.destLeft = data->DestLeft; p.destTop = data->DestTop;
    // A provider alias is sequential in the software implementation, even if
    // destination COW would produce a distinct texture from its old image.
    if(data->Dest == data->Src1 || data->Dest == data->Src2) {
        krkrsdl3::layer_work::RecordTransitionResult(false, "targetAlias",
            p.width > 0 && p.height > 0 ? static_cast<std::uint64_t>(p.width) * p.height : 0);
        return TVPLayerTransitionResult::Alias;
    }
    // Capture sources before GetTextureForRender performs destination COW.
    auto *source1 = data->Src1->GetTexture();
    auto *source2 = data->Src2->GetTexture();
    auto *target = data->Dest->GetTextureForRender();
    // Distinct providers can still refer to the same bitmap object. Software
    // acquires destination-for-write first, then reads these current sources;
    // refresh after COW so the facade can reject an actual resulting alias.
    source1 = data->Src1->GetTexture();
    source2 = data->Src2->GetTexture();
    return TVPTryMetalLayerTransition(operation, target, source1, source2);
}
