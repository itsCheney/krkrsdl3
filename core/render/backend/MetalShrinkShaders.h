#pragma once
#include <string>

inline constexpr char TVP_SHRINK_MSL_DEFINITIONS[] =
#define TVP_LAYER_SHRINK(name,id) "constant int TVP_SHRINK_" #name "=" #id ";\n"
#define TVP_LAYER_SHRINK_COUNT(count)
#include "../LayerShrinkDefinitions.def"
#undef TVP_LAYER_SHRINK_COUNT
#undef TVP_LAYER_SHRINK
;

inline constexpr char kMetalShrinkShaders[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
#if TVP_SHRINK_WIDE
typedef ulong ShrinkAvg;
#else
typedef uint ShrinkAvg;
#endif
struct ShrinkAxis { int base,step; ShrinkAvg ta,tc,ba,bc,total; };
struct ShrinkParams { int kind,width,height,sourceTop,sourceRows; uint hu,vu; };
// BEGIN SHRINK SCALAR: production helpers extracted by the portable oracle.
struct ShrinkSum { ShrinkAvg c[4]; };
uint shrinkRead(texture2d<float,access::read> source,int x,int y) {
    uint4 bytes=uint4(round(source.read(uint2(x,y))*255.0));
    return bytes.r|(bytes.g<<8)|(bytes.b<<16)|(bytes.a<<24);
}
void shrinkAdd(thread ShrinkSum& sum,uint value,ShrinkAvg color,ShrinkAvg alpha) {
    for(int c=0;c<4;++c) sum.c[c]+=ShrinkAvg((value>>(c*8))&255u)*(c==3?alpha:color);
}
ShrinkSum shrinkHorizontal(const device ShrinkAxis& axis,int y,constant ShrinkParams& p,
        texture2d<float,access::read> source) {
    ShrinkSum sum={{0,0,0,0}};
    if(p.kind==TVP_SHRINK_Fast) {
        for(int i=0;i<axis.step;++i) shrinkAdd(sum,shrinkRead(source,axis.base+i,y),1,0);
        for(int c=0;c<3;++c) sum.c[c]/=ShrinkAvg(axis.step);
        sum.c[3]=255;
        return sum;
    }
    // Alpha controls the read, even when its RGB coefficient differs.
    if(axis.ta) shrinkAdd(sum,shrinkRead(source,axis.base-1,y),ShrinkAvg(axis.tc),ShrinkAvg(axis.ta));
    for(int i=0;i<axis.step;++i) shrinkAdd(sum,shrinkRead(source,axis.base+i,y),ShrinkAvg(p.hu),ShrinkAvg(p.hu));
    if(axis.ba) shrinkAdd(sum,shrinkRead(source,axis.base+axis.step,y),ShrinkAvg(axis.bc),ShrinkAvg(axis.ba));
    return sum;
}
void shrinkAddRow(thread ShrinkSum& sum,const device ShrinkSum* rows,int x,int sourceY,
        constant ShrinkParams& p,ShrinkAvg color,ShrinkAvg alpha) {
    const device ShrinkSum& value=rows[(sourceY-p.sourceTop)*p.width+x];
    for(int c=0;c<4;++c) sum.c[c]+=value.c[c]*(c==3?alpha:color);
}
uint shrinkVertical(int x,const device ShrinkAxis& horizontal,const device ShrinkAxis& vertical,
        constant ShrinkParams& p,const device ShrinkSum* rows) {
    ShrinkSum sum={{0,0,0,0}};
    ShrinkAvg divisor;
    if(p.kind==TVP_SHRINK_Fast) {
        for(int i=0;i<vertical.step;++i) shrinkAddRow(sum,rows,x,vertical.base+i,p,1,0);
        divisor=ShrinkAvg(vertical.step);sum.c[3]=255*divisor;
    } else {
        if(vertical.ta) shrinkAddRow(sum,rows,x,vertical.base-1,p,ShrinkAvg(vertical.tc),ShrinkAvg(vertical.ta));
        for(int i=0;i<vertical.step;++i) shrinkAddRow(sum,rows,x,vertical.base+i,p,ShrinkAvg(p.vu),ShrinkAvg(p.vu));
        if(vertical.ba) shrinkAddRow(sum,rows,x,vertical.base+vertical.step,p,ShrinkAvg(vertical.bc),ShrinkAvg(vertical.ba));
        divisor=ShrinkAvg(horizontal.total)*ShrinkAvg(vertical.total);
    }
    uint result=0;
    for(int c=0;c<4;++c) result|=uint((sum.c[c]/divisor)&255u)<<(c*8);
    return result;
}
// END SHRINK SCALAR
kernel void shrinkHorizontalLayer(uint2 tid [[thread_position_in_grid]],constant ShrinkParams& p [[buffer(0)]],
        const device ShrinkAxis* axes [[buffer(1)]],device ShrinkSum* output [[buffer(2)]],
        texture2d<float,access::read> source [[texture(0)]]) {
    if(int(tid.x)>=p.width || int(tid.y)>=p.sourceRows) return;
    output[tid.y*p.width+tid.x]=shrinkHorizontal(axes[tid.x],p.sourceTop+int(tid.y),p,source);
}
kernel void shrinkVerticalLayer(uint2 tid [[thread_position_in_grid]],constant ShrinkParams& p [[buffer(0)]],
        const device ShrinkAxis* horizontal [[buffer(1)]],const device ShrinkAxis* vertical [[buffer(2)]],
        const device ShrinkSum* rows [[buffer(3)]],texture2d<float,access::write> output [[texture(0)]]) {
    if(int(tid.x)>=p.width || int(tid.y)>=p.height) return;
    uint value=shrinkVertical(int(tid.x),horizontal[tid.x],vertical[tid.y],p,rows);
    output.write(float4(float(value&255u),float((value>>8)&255u),float((value>>16)&255u),float(value>>24))/255.0,tid);
}
)MSL";
inline std::string TVPBuildMetalShrinkShaderSource(bool wide) {
    return std::string(wide ? "#define TVP_SHRINK_WIDE 1\n" : "#define TVP_SHRINK_WIDE 0\n")+
        TVP_SHRINK_MSL_DEFINITIONS+kMetalShrinkShaders;
}
