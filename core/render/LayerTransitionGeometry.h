#pragma once
#include "LayerTransition.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace layer_transition {
inline constexpr size_t MaxParameterBytes=64*1024*1024;
inline int32_t I32(const TVPLayerTransitionBytes& b,size_t i) {
    int32_t v; std::memcpy(&v,b.data()+i*4,4); return v;
}
inline uint16_t U16(const TVPLayerTransitionBytes& b,size_t i) {
    uint16_t v; std::memcpy(&v,b.data()+i*2,2); return v;
}
inline bool Fixed(int start,int step,int64_t delta,int& pixel) {
    const int64_t v=int64_t(start)+int64_t(step)*delta;
    if(v<std::numeric_limits<int32_t>::min() || v>std::numeric_limits<int32_t>::max()) return false;
    pixel=int(v>>16); return true;
}
// Shared preflight for the production facade, native backend and device double.
// Table scans may be skipped ONLY for an already validated immutable cache hit.
inline TVPLayerTransitionResult Validate(const TVPLayerTransitionOperation& op,
        int tw,int th,int sw1,int sh1,int sw2,int sh2,bool scanTable=true) {
    const auto& p=op.params;
    if(p.kind<=0 || p.kind>=int(TVPLayerTransitionKind::Count)) return TVPLayerTransitionResult::Unsupported;
    if(p.frameWidth<=0 || p.frameHeight<=0 || p.width<0 || p.height<0 ||
       p.left<0 || p.top<0 || int64_t(p.left)+p.width>p.frameWidth ||
       int64_t(p.top)+p.height>p.frameHeight || p.destLeft<0 || p.destTop<0 ||
       int64_t(p.destLeft)+p.width>tw || int64_t(p.destTop)+p.height>th ||
       sw1<p.frameWidth || sw2<p.frameWidth || sh1<p.frameHeight || sh2<p.frameHeight)
        return TVPLayerTransitionResult::InvalidGeometry;
    if(p.ratio<0 || p.ratio>255) return TVPLayerTransitionResult::InvalidParameters;
    const size_t tableBytes=op.table ? op.table->size() : 0,rowsBytes=op.rows ? op.rows->size() : 0;
    if(tableBytes>MaxParameterBytes || rowsBytes>MaxParameterBytes-tableBytes)
        return TVPLayerTransitionResult::AllocationFailed;
    if(!p.width || !p.height) return TVPLayerTransitionResult::Applied;
    if(p.kind==int(TVPLayerTransitionKind::Mosaic)) {
        if(p.blockSize<=0 || p.offsetX>0 || p.offsetY>0 ||
           int64_t(p.offsetX)<=-int64_t(p.blockSize) || int64_t(p.offsetY)<=-int64_t(p.blockSize))
            return TVPLayerTransitionResult::InvalidParameters;
        for(int axis=0;axis<2;++axis) {
            const int offset=axis?p.offsetY:p.offsetX,start=axis?p.top:p.left,size=axis?p.height:p.width;
            for(int x:{start,start+size-1}) {
                const int64_t distance=int64_t(x)-offset;
                const int64_t base=(distance/p.blockSize)*p.blockSize;
                if(distance>INT32_MAX || base>INT32_MAX || base+offset<INT32_MIN ||
                   base+offset+p.blockSize/2>INT32_MAX) return TVPLayerTransitionResult::InvalidParameters;
            }
        }
    } else if(p.kind==int(TVPLayerTransitionKind::Wave)) {
        if(p.flags>2 || rowsBytes!=size_t(p.height)*4) return TVPLayerTransitionResult::InvalidParameters;
        for(int y=0;y<p.height;++y) {
            const int64_t d=I32(*op.rows,y);
            if(int64_t(p.left)-d<std::numeric_limits<int32_t>::min() ||
               int64_t(p.left)+p.width-1-d>std::numeric_limits<int32_t>::max())
                return TVPLayerTransitionResult::InvalidParameters;
        }
    } else if(p.kind==int(TVPLayerTransitionKind::Ripple)) {
        if(p.centerX<0 || p.centerX>=p.frameWidth || p.centerY<0 || p.centerY>=p.frameHeight ||
           p.mapWidth!=std::max(p.centerX,p.frameWidth-p.centerX) ||
           p.mapHeight!=std::max(p.centerY,p.frameHeight-p.centerY) ||
           (p.blockSize!=16 && p.blockSize!=32 && p.blockSize!=64 && p.blockSize!=128) || tableBytes%2)
            return TVPLayerTransitionResult::InvalidParameters;
        const uint64_t map=uint64_t(p.mapWidth)*p.mapHeight,words=tableBytes/2;
        if(map>words || p.driftOffset<0 || uint64_t(p.driftOffset)<map ||
           uint64_t(p.driftOffset)+p.blockSize*32>words) return TVPLayerTransitionResult::InvalidParameters;
        if(scanTable) {
            for(size_t i=0;i<map;++i) if(U16(*op.table,i)>=p.blockSize*32)
                return TVPLayerTransitionResult::InvalidParameters;
            for(size_t i=map;i<words;++i) {
                const auto drift=U16(*op.table,i);
                int dx=int(drift>>8),dy=int(drift&255);
                if(dx>=128) dx-=256; if(dy>=128) dy-=256;
                if(std::abs(dx)>=p.frameWidth || std::abs(dy)>=p.frameHeight)
                    return TVPLayerTransitionResult::InvalidParameters;
            }
        }
    } else if(p.kind==int(TVPLayerTransitionKind::Turn)) {
        if(tableBytes!=(64*64*8+64)*4 || p.offsetX!=2) return TVPLayerTransitionResult::InvalidParameters;
        for(int bx:{p.left/64,(p.left+p.width-1)/64}) for(int by:{p.top/64,(p.top+p.height-1)/64}) {
            const int64_t raw=int64_t(p.phase)-int64_t(bx-by)*p.offsetX;
            if(raw<INT32_MIN || raw>INT32_MAX) return TVPLayerTransitionResult::InvalidParameters;
        }
        const auto& table=*op.table;
        for(int y=p.top;y<p.top+p.height;++y) for(int bx=p.left/64;bx<=(p.left+p.width-1)/64;++bx) {
            const int64_t raw=int64_t(p.phase)-int64_t(bx-y/64)*p.offsetX;
            int phase=int(std::clamp<int64_t>(raw,0,63));
            if(phase==0 || phase==63) continue;
            const size_t i=size_t(phase*64+y%64)*8;
            int start=I32(table,i),len=I32(table,i+1);
            if(start<0 || len<0 || int64_t(start)+len>64) return TVPLayerTransitionResult::InvalidParameters;
            const int l=std::max(p.left-bx*64,start),r=std::min(p.left+p.width-bx*64,start+len);
            for(int x:{l,r-1}) if(l<r) {
                int sx,sy;
                if(!Fixed(I32(table,i+2),I32(table,i+6),x-start,sx) ||
                   !Fixed(I32(table,i+3),I32(table,i+7),x-start,sy)) return TVPLayerTransitionResult::InvalidParameters;
                sx+=bx*64; sy+=(y/64)*64;
                if(sy>=p.frameHeight) continue; // original bottom-block background
                const int sw=phase<32?sw1:sw2,sh=phase<32?sh1:sh2;
                if(sx<0 || sy<0 || sx>=sw || sy>=sh) return TVPLayerTransitionResult::InvalidGeometry;
            }
            const int gloss=I32(table,64*64*8+phase);
            if(gloss<0 || gloss>255) return TVPLayerTransitionResult::InvalidParameters;
        }
    } else {
        if(rowsBytes!=size_t(p.frameHeight)*26*4) return TVPLayerTransitionResult::InvalidParameters;
        const auto& rows=*op.rows;
        for(int y=p.top;y<p.top+p.height;++y) {
            const size_t base=size_t(y)*26;
            const int count=I32(rows,base);
            if(count<1 || count>5) return TVPLayerTransitionResult::InvalidParameters;
            std::array<std::pair<int,int>,5> coverage{};
            for(int j=0;j<count;++j) {
                const size_t r=base+11+j*3;
                int left=I32(rows,r),right=I32(rows,r+1),type=I32(rows,r+2);
                if(type<0 || type>2 || left<0 || right<left || right>p.frameWidth)
                    return TVPLayerTransitionResult::InvalidParameters;
                coverage[j]={left,right};
                if(type==0) continue;
                const int l=std::max(p.left,left),rr=std::min(p.left+p.width,right);
                const size_t line=base+(type==1?1:6);
                for(int x:{l,rr-1}) if(l<rr) {
                    int sx,sy;
                    if(!Fixed(I32(rows,line+1),I32(rows,line+3),int64_t(x)-I32(rows,line),sx) ||
                       !Fixed(I32(rows,line+2),I32(rows,line+4),int64_t(x)-I32(rows,line),sy))
                        return TVPLayerTransitionResult::InvalidParameters;
                    if(sx<0 || sy<0 || sx>=(type==1?sw1:sw2) || sy>=(type==1?sh1:sh2))
                        return TVPLayerTransitionResult::InvalidGeometry;
                }
            }
            std::sort(coverage.begin(),coverage.begin()+count);
            int covered=p.left;
            for(int i=0;i<count;++i) {
                if(coverage[i].second<=covered) continue;
                if(coverage[i].first>covered) break;
                covered=coverage[i].second;
            }
            if(covered<p.left+p.width) return TVPLayerTransitionResult::InvalidParameters;
        }
    }
    return TVPLayerTransitionResult::Applied;
}
}
