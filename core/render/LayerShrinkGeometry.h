#pragma once
#include "LayerShrink.h"
#include <algorithm>
#include <limits>

namespace TVPLayerShrinkGeometry {
constexpr uint64_t ParameterBudget = 64ull * 1024 * 1024;
struct Span { int64_t first = 0, last = -1; bool Empty() const { return last < first; } };
struct Validation {
    int32_t sourceTop = 0, sourceRows = 0;
    uint64_t parameterBytes = 0, temporaryBytes = 0;
    bool safe32 = false;
};
inline bool Add(uint64_t a,uint64_t b,uint64_t& out) {
    if(a>std::numeric_limits<uint64_t>::max()-b) return false;
    out=a+b; return true;
}
inline bool Multiply(uint64_t a,uint64_t b,uint64_t& out) {
    if(b && a>std::numeric_limits<uint64_t>::max()/b) return false;
    out=a*b; return true;
}
inline Span ReadSpan(const TVPLayerShrinkAxis& axis,TVPLayerShrinkKind kind) {
    if(kind==TVPLayerShrinkKind::Fast) return {axis.base,int64_t(axis.base)+axis.step-1};
    Span result;
    const auto include=[&](int64_t a,int64_t b) {
        if(result.Empty()) result={a,b};
        else { result.first=std::min(result.first,a); result.last=std::max(result.last,b); }
    };
    if(axis.ta) include(int64_t(axis.base)-1,int64_t(axis.base)-1);
    if(axis.step>0) include(axis.base,int64_t(axis.base)+axis.step-1);
    if(axis.ba) include(int64_t(axis.base)+axis.step,int64_t(axis.base)+axis.step);
    return result;
}
inline bool Intersects(const Span& s,int64_t first,int64_t end) {
    return !s.Empty() && s.first<end && s.last>=first && first<end;
}
// Factor the row-major dependency test into independent axis predicates. The
// source can overlap the current/future output, but never an earlier CPU write.
inline bool AliasSafe(const TVPLayerShrinkOperation& op) {
    bool xHitsOutput=false, xHitsEarlier=false;
    for(size_t x=0;x<op.horizontal->size();++x) {
        const auto span=ReadSpan((*op.horizontal)[x],op.kind);
        xHitsOutput=xHitsOutput || Intersects(span,op.destination.left,op.destination.right);
        xHitsEarlier=xHitsEarlier || Intersects(span,op.destination.left,int64_t(op.destination.left)+x);
    }
    for(size_t y=0;y<op.vertical->size();++y) {
        const auto span=ReadSpan((*op.vertical)[y],op.kind);
        const int64_t row=int64_t(op.destination.top)+y;
        if(xHitsOutput && Intersects(span,op.destination.top,row)) return false;
        if(xHitsEarlier && Intersects(span,row,row+1)) return false;
    }
    return true;
}
inline bool AxisSafe32(const TVPLayerShrinkAxis& a,uint64_t unit,uint64_t& maxWeight) {
    uint64_t middle=0, rgb=0, alpha=0;
    if(!Multiply(uint64_t(std::max(a.step,0)),unit,middle)) return false;
    if(!Add(a.ta ? a.tc : 0,a.ba ? a.bc : 0,rgb) || !Add(rgb,middle,rgb) ||
       !Add(a.ta,a.ba,alpha) || !Add(alpha,middle,alpha)) return false;
    maxWeight=std::max(rgb,alpha);
    return maxWeight<=UINT32_MAX && a.total<=UINT32_MAX;
}
inline TVPLayerShrinkResult Validate(const TVPLayerShrinkOperation& op,int sw,int sh,
                                     int dw,int dh,bool alias,Validation& out) {
    out={};
    if(op.kind!=TVPLayerShrinkKind::Area && op.kind!=TVPLayerShrinkKind::Fast)
        return TVPLayerShrinkResult::Unsupported;
    const int64_t w=int64_t(op.destination.right)-op.destination.left;
    const int64_t h=int64_t(op.destination.bottom)-op.destination.top;
    if(sw<=0 || sh<=0 || dw<=0 || dh<=0 || w<=0 || h<=0 ||
       op.destination.left<0 || op.destination.top<0 || op.destination.right>dw ||
       op.destination.bottom>dh || !op.horizontal || !op.vertical ||
       op.horizontal->size()!=uint64_t(w) || op.vertical->size()!=uint64_t(h))
        return TVPLayerShrinkResult::Geometry;
    if((op.avgBits!=32 && op.avgBits!=64) || !op.hu || !op.vu || op.hu>256 || op.vu>256)
        return TVPLayerShrinkResult::Arithmetic;
    uint64_t maxX=0,maxY=0,maxTotalX=0,maxTotalY=0;
    bool safe32=true;
    Span rows;
    const auto axes=[&](const std::vector<TVPLayerShrinkAxis>& list,int bound,uint64_t unit,
                        uint64_t& maxWeight,uint64_t& maxTotal,bool vertical) {
        for(const auto& a:list) {
            if(a.step < (op.kind==TVPLayerShrinkKind::Area ? -1 : 1) || !a.total ||
               (op.kind==TVPLayerShrinkKind::Area && (a.ta>256 || a.ba>256)) ||
               (op.avgBits==32 && (a.total>UINT32_MAX || a.tc>UINT32_MAX || a.bc>UINT32_MAX)))
                return false;
            const Span span=ReadSpan(a,op.kind);
            if(!span.Empty() && (span.first<0 || span.last>=bound)) return false;
            if(vertical && !span.Empty()) {
                if(rows.Empty()) rows=span;
                else { rows.first=std::min(rows.first,span.first); rows.last=std::max(rows.last,span.last); }
            }
            uint64_t weight=0;
            if(op.kind==TVPLayerShrinkKind::Fast) weight=uint64_t(a.step);
            else if(!AxisSafe32(a,unit,weight)) safe32=false;
            maxWeight=std::max(maxWeight,weight); maxTotal=std::max(maxTotal,a.total);
        }
        return true;
    };
    if(!axes(*op.horizontal,sw,op.hu,maxX,maxTotalX,false) ||
       !axes(*op.vertical,sh,op.vu,maxY,maxTotalY,true)) return TVPLayerShrinkResult::Arithmetic;
    // The divisor itself follows AvgT modular multiplication. Check each pair
    // without an O(W*H) scan: a zero modulo 2^32 requires trailing zeros >=32.
    unsigned maxZerosX=0,maxZerosY=0;
    const auto zeros=[](uint64_t v) { unsigned n=0; while(v && !(v&1)) {++n;v>>=1;} return n; };
    for(const auto& a:*op.horizontal) maxZerosX=std::max(maxZerosX,zeros(a.total));
    for(const auto& a:*op.vertical) maxZerosY=std::max(maxZerosY,zeros(a.total));
    if(op.kind==TVPLayerShrinkKind::Area && maxZerosX+maxZerosY>=op.avgBits)
        return TVPLayerShrinkResult::Arithmetic;
    uint64_t bound=0,total=0;
    safe32=safe32 && Multiply(maxX,maxY,bound) && Multiply(bound,255,bound) &&
           bound<=UINT32_MAX && Multiply(maxTotalX,maxTotalY,total) && total<=UINT32_MAX;
    out.safe32=op.kind==TVPLayerShrinkKind::Fast || op.avgBits==32 || safe32;
    out.sourceTop=rows.Empty()?0:int32_t(rows.first);
    out.sourceRows=rows.Empty()?1:int32_t(rows.last-rows.first+1);
    uint64_t axesCount=0, intermediates=0,pixels=0,totalBytes=0;
    const uint64_t elementBytes=op.kind==TVPLayerShrinkKind::Fast ? 16 : (out.safe32 ? 16 : 32);
    if(!Add(uint64_t(w),uint64_t(h),axesCount) || !Multiply(axesCount,sizeof(TVPLayerShrinkAxis),out.parameterBytes) ||
       !Multiply(uint64_t(w),uint64_t(out.sourceRows),intermediates) || !Multiply(intermediates,elementBytes,intermediates) ||
       !Multiply(uint64_t(w),uint64_t(h),pixels) || !Multiply(pixels,4,pixels) ||
       !Add(intermediates,pixels,out.temporaryBytes) || !Add(out.parameterBytes,out.temporaryBytes,totalBytes))
        return TVPLayerShrinkResult::ParameterBudget;
    if(totalBytes>ParameterBudget) return TVPLayerShrinkResult::ParameterBudget;
    if(alias && !AliasSafe(op)) return TVPLayerShrinkResult::AliasDependency;
    return TVPLayerShrinkResult::Applied;
}
inline TVPLayerShrinkResult Validate(const TVPLayerShrinkOperation& op,int tw,int th,
                                     int sw,int sh,bool alias) {
    Validation result;
    const auto status=Validate(op,sw,sh,tw,th,alias,result);
    if(status==TVPLayerShrinkResult::Applied &&
       (op.sourceTop!=result.sourceTop || op.sourceRows!=result.sourceRows))
        return TVPLayerShrinkResult::Geometry;
    return status;
}
inline bool CanUse32(const TVPLayerShrinkOperation& op) {
    if(op.kind==TVPLayerShrinkKind::Fast || op.avgBits==32) return true;
    if(!op.horizontal || !op.vertical) return false;
    uint64_t x=0,y=0,tx=0,ty=0,bound=0,total=0;
    for(const auto& a:*op.horizontal) { uint64_t weight=0;
        if(!AxisSafe32(a,op.hu,weight)) return false;
        x=std::max(x,weight); tx=std::max(tx,a.total); }
    for(const auto& a:*op.vertical) { uint64_t weight=0;
        if(!AxisSafe32(a,op.vu,weight)) return false;
        y=std::max(y,weight); ty=std::max(ty,a.total); }
    return Multiply(x,y,bound) && Multiply(bound,255,bound) && bound<=UINT32_MAX &&
           Multiply(tx,ty,total) && total<=UINT32_MAX;
}
inline uint64_t TemporaryBytes(const TVPLayerShrinkOperation& op,uint32_t selectedBits) {
    if(!op.horizontal || !op.vertical || op.sourceRows<=0 ||
       (selectedBits!=32 && selectedBits!=64)) return 0;
    uint64_t a=0,b=0,result=0;
    const uint64_t bytes=op.kind==TVPLayerShrinkKind::Fast ? 16 : selectedBits/8*4;
    if(!Multiply(op.horizontal->size(),uint64_t(op.sourceRows),a) || !Multiply(a,bytes,a) ||
       !Multiply(op.horizontal->size(),op.vertical->size(),b) || !Multiply(b,4,b) ||
       !Add(a,b,result)) return 0;
    return result;
}
} // namespace TVPLayerShrinkGeometry
