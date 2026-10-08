#pragma once
#include <string>

inline constexpr char TVP_SPAN_MSL_DEFINITIONS[] =
#define TVP_LAYER_SPAN(name,id) "constant uint TVP_SPAN_" #name "=" #id "u;\n"
#define TVP_LAYER_SPAN_COUNT(count)
#include "../LayerSpanCompositeDefinitions.def"
#undef TVP_LAYER_SPAN_COUNT
#undef TVP_LAYER_SPAN
;
inline constexpr char kMetalSpanCompositeShaders[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Span { int x,y; uint length,coverage,kind,solid,sourceOffset,reserved; };
struct SpanParams { int left,top; uint width,height,rowWords,reserved; };
// BEGIN SPAN SCALAR
// Derived from plutovg v1.3.3 source/plutovg-blend.c (MIT):
// BYTE_MUL, composition_solid_source, composition_solid_source_over,
// composition_source_over. Packed uint addition deliberately preserves carries
// and 32-bit wrap even for arbitrary non-premultiplied test input.
uint spanByteMul(uint x,uint a) {
    uint t=(x&0xff00ffu)*a;
    t=(t+((t>>8)&0xff00ffu)+0x800080u)>>8;
    t&=0xff00ffu;
    x=((x>>8)&0xff00ffu)*a;
    x=x+((x>>8)&0xff00ffu)+0x800080u;
    x&=0xff00ff00u;
    return x|t;
}
uint spanComposite(uint destination,uint source,uint coverage,uint kind) {
    if(kind==TVP_SPAN_SolidSource) {
        if(coverage==255u) return source;
        return spanByteMul(source,coverage)+spanByteMul(destination,255u-coverage);
    }
    if(kind==TVP_SPAN_SolidSourceOver) {
        if(coverage!=255u) source=spanByteMul(source,coverage);
        return source+spanByteMul(destination,255u-(source>>24));
    }
    // ArraySourceOver has its own upstream opaque/zero fast paths.
    if(coverage==255u) {
        if(source>=0xff000000u) return source;
        if(source==0u) return destination;
    } else source=spanByteMul(source,coverage);
    return source+spanByteMul(destination,(~source)>>24);
}
uint spanComposePixel(uint oldPixel,int x,int y,uint row,
        const device Span* spans,const device uint* sourcePixels,
        const device uint* rowOffsets,const device uint* rowEntries) {
    (void)y;
    uint value=oldPixel;
    for(uint n=rowOffsets[row];n<rowOffsets[row+1];++n) {
        const device Span& span=spans[rowEntries[n]];
        if(x<span.x || uint(x-span.x)>=span.length) continue;
        uint source=span.kind==TVP_SPAN_ArraySourceOver ? sourcePixels[span.sourceOffset+uint(x-span.x)] : span.solid;
        value=spanComposite(value,source,span.coverage,span.kind);
    }
    return value;
}
// END SPAN SCALAR
kernel void compositeLayerSpans(uint2 tid [[thread_position_in_grid]],
        constant SpanParams& p [[buffer(0)]],const device Span* spans [[buffer(1)]],
        const device uint* sourcePixels [[buffer(2)]],const device uint* rowOffsets [[buffer(3)]],
        const device uint* rowEntries [[buffer(4)]],const device uint* previous [[buffer(5)]],
        device uint* output [[buffer(6)]]) {
    if(tid.x>=p.width || tid.y>=p.height) return;
    const uint offset=tid.y*p.rowWords+tid.x;
    output[offset]=spanComposePixel(previous[offset],p.left+int(tid.x),p.top+int(tid.y),tid.y,
        spans,sourcePixels,rowOffsets,rowEntries);
}
)MSL";
inline std::string TVPBuildMetalSpanCompositeShaderSource() {
    return std::string(TVP_SPAN_MSL_DEFINITIONS)+kMetalSpanCompositeShaders;
}
