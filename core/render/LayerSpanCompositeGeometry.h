#pragma once
#include "LayerSpanComposite.h"
#include <limits>
#include <new>

struct TVPLayerSpanCompositeGeometry {
    using Result=TVPLayerSpanCompositeResult;
    static constexpr uint64_t ParameterBudget=64ull*1024*1024;
    static constexpr uint64_t UniformBytes=24;
    static constexpr size_t MaxSpans=1048576;
    static constexpr uint32_t MaxRowReferences=512;
    static uint64_t ScratchRowBytes(const TVPLayerSpanCompositePacket& p) {
        const uint64_t width=uint64_t(int64_t(p.destination.right)-p.destination.left);
        return (width*4+255)&~uint64_t(255);
    }
    static uint64_t ParameterBytes(const TVPLayerSpanCompositePacket& p) {
        return uint64_t(p.spans.size())*sizeof(TVPLayerSpan)+
            (uint64_t(p.sourcePixels.size())+p.rowOffsets.size()+p.rowEntries.size())*4+UniformBytes;
    }
    // The existing staging pool may provide a larger buffer than requested.
    // Bound its actual retained allocation, together with both private surfaces.
    static bool StagedBudgetFits(uint64_t parameterAllocationBytes,uint64_t scratchBytes) {
        return parameterAllocationBytes<=ParameterBudget-UniformBytes &&
            scratchBytes<=(ParameterBudget-UniformBytes-parameterAllocationBytes)/2;
    }
    static Result ValidateSpans(const TVPLayerSpanCompositePacket& p,uint32_t width,uint32_t height) {
        const auto& r=p.destination;
        if(r.left<0 || r.top<0 || r.right<r.left || r.bottom<r.top ||
           uint64_t(r.right)>width || uint64_t(r.bottom)>height) return Result::Geometry;
        if(p.spans.size()>MaxSpans || p.sourcePixels.size()>UINT32_MAX) return Result::ParameterBudget;
        const uint64_t h=uint64_t(int64_t(r.bottom)-r.top);
        // Two raw-byte scratch surfaces: source snapshot and independent output.
        if((h && ScratchRowBytes(p)>ParameterBudget/2/h) ||
           uint64_t(p.spans.size())*32+uint64_t(p.sourcePixels.size())*4>ParameterBudget)
            return Result::ParameterBudget;
        for(const auto& s:p.spans) {
            if(s.kind==0 || s.kind>=uint32_t(TVPLayerSpanKind::Count)) return Result::Unsupported;
            if(!s.length || s.coverage>255 || s.reserved || s.x<r.left || s.y<r.top ||
               s.y>=r.bottom || int64_t(s.x)+s.length>r.right) return Result::Geometry;
            if(s.kind==uint32_t(TVPLayerSpanKind::ArraySourceOver) &&
               uint64_t(s.sourceOffset)+s.length>p.sourcePixels.size()) return Result::Resource;
        }
        return Result::Applied;
    }
    static Result Validate(const TVPLayerSpanCompositePacket& p,uint32_t width,uint32_t height) {
        auto result=ValidateSpans(p,width,height);if(result!=Result::Applied) return result;
        const uint64_t h=uint64_t(int64_t(p.destination.bottom)-p.destination.top);
        if(p.rowOffsets.size()!=h+1 || p.rowEntries.size()!=p.spans.size() ||
           p.rowOffsets.empty() || p.rowOffsets[0]!=0 || p.rowOffsets.back()!=p.rowEntries.size())
            return Result::Geometry;
        if(ParameterBytes(p)+ScratchRowBytes(p)*h*2>ParameterBudget) return Result::ParameterBudget;
        for(uint64_t row=0;row<h;++row) {
            const uint32_t start=p.rowOffsets[row],end=p.rowOffsets[row+1];
            if(end<start || end>p.rowEntries.size()) return Result::Geometry;
            if(end-start>MaxRowReferences) return Result::ParameterBudget;
            for(uint32_t j=start;j<end;++j) {
                const uint32_t i=p.rowEntries[j];
                if(i>=p.spans.size() || (j>start && i<=p.rowEntries[j-1]) ||
                   int64_t(p.spans[i].y)!=int64_t(p.destination.top)+int64_t(row)) return Result::Geometry;
            }
        }
        return Result::Applied;
    }
    static Result PrepareRows(TVPLayerSpanCompositePacket& p,uint32_t width,uint32_t height) {
        auto result=ValidateSpans(p,width,height);if(result!=Result::Applied) return result;
        const size_t h=size_t(int64_t(p.destination.bottom)-p.destination.top);
        if(uint64_t(h+1)*4+uint64_t(p.spans.size())*36+uint64_t(p.sourcePixels.size())*4+
           ScratchRowBytes(p)*h*2+UniformBytes>ParameterBudget) return Result::ParameterBudget;
        try {
            p.rowOffsets.assign(h+1,0);p.rowEntries.resize(p.spans.size());
            for(const auto& s:p.spans) {
                auto& n=p.rowOffsets[size_t(s.y-p.destination.top)+1];
                if(++n>MaxRowReferences) return Result::ParameterBudget;
            }
            for(size_t i=1;i<=h;++i) p.rowOffsets[i]+=p.rowOffsets[i-1];
            // Fill backwards, avoiding an extra row cursor
            // allocation while preserving the exact original span call order.
            for(size_t i=p.spans.size();i>0;--i) {
                const size_t row=size_t(p.spans[i-1].y-p.destination.top);
                p.rowEntries[--p.rowOffsets[row+1]]=uint32_t(i-1);
            }
            for(size_t row=0;row<h;++row) p.rowOffsets[row]=p.rowOffsets[row+1];
            p.rowOffsets[h]=uint32_t(p.spans.size());
        } catch(const std::bad_alloc&) {return Result::ParameterBudget;}
        return Validate(p,width,height);
    }
};
