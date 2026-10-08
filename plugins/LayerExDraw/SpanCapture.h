#pragma once
#include "LayerSpanComposite.h"
#include "LayerSpanCompositeGeometry.h"
#include <plutovg.h>
#include <limits>

// The original plutovg blend loop supplies final, premultiplied paint samples.
// Copy every borrowed chunk now: texture rows and stack buffers never escape.
class TVPPlutovgSpanCapture {
public:
    TVPLayerSpanCompositePacket packet;
    bool failed=false;
    static bool Append(void* closure,plutovg_span_kind_t kind,int x,int y,int length,
        int coverage,uint32_t solid,const uint32_t* source) noexcept {
        auto& self=*static_cast<TVPPlutovgSpanCapture*>(closure);
        if(self.failed) return false;
        constexpr size_t MaxBytes=64u*1024u*1024u;
        if(length<=0 || x<0 || y<0 || coverage<0 || coverage>255 ||
            uint32_t(kind)<uint32_t(TVPLayerSpanKind::SolidSource) ||
            uint32_t(kind)>uint32_t(TVPLayerSpanKind::ArraySourceOver)) return !(self.failed=true);
        const bool array=kind==PLUTOVG_SPAN_ARRAY_SOURCE_OVER;
        if(array && !source) return !(self.failed=true);
        const size_t pixels=array ? size_t(length) : 0;
        if(pixels>MaxBytes/4 || self.packet.sourcePixels.size()>MaxBytes/4-pixels ||
            self.packet.spans.size()>=MaxBytes/sizeof(TVPLayerSpan) ||
            (self.packet.spans.size()+1)*sizeof(TVPLayerSpan)+
            (self.packet.sourcePixels.size()+pixels)*4>MaxBytes) return !(self.failed=true);
        try {
            TVPLayerSpan span;
            span.x=x;span.y=y;span.length=uint32_t(length);span.coverage=uint32_t(coverage);
            span.kind=uint32_t(kind);span.solid=solid;
            span.sourceOffset=uint32_t(self.packet.sourcePixels.size());
            if(array) self.packet.sourcePixels.insert(self.packet.sourcePixels.end(),source,source+length);
            self.packet.spans.push_back(span);
            return true;
        } catch(...) { self.failed=true;return false; }
    }
};
