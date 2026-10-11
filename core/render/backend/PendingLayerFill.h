#pragma once
#include "../LayerRenderOperation.h"
#include <cstdint>

namespace krkrsdl3 { namespace pending_fill {
struct Counters {uint64_t deferred=0,materialized=0,omitted=0;};
inline bool Covers(const TVPLayerRect& outer,const TVPLayerRect& inner) noexcept {
    return outer.left<=inner.left && outer.top<=inner.top && outer.right>=inner.right && outer.bottom>=inner.bottom;
}
// Called only after the incoming operation's validation and binding resources
// have succeeded. Geometry uses the clipped write region, never the source.
inline bool CanOmit(const TVPLayerOperation& op,const TVPLayerRect& clip,
                    const TVPLayerRect& pending,int width,int height,bool alias) noexcept {
    if(op.flags || alias)return false;
    if(op.kind==TVPLayerOperationKind::Fill)return Covers(clip,pending);
    return op.kind==TVPLayerOperationKind::Copy && clip.left==0 && clip.top==0 &&
        clip.right==width && clip.bottom==height;
}
template<class Parameters> struct Slot {
    void* target=nullptr;
    TVPLayerRect region{};
    Parameters parameters{};
    uint64_t logicalID=0;
    explicit operator bool() const noexcept {return target!=nullptr;}
    void Reset() noexcept {target=nullptr;logicalID=0;}
};
} }
