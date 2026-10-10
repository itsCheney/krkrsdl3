#pragma once
#include <string>
#include "../LayerOperationShaderDefinitions.h"

// Integer byte formulas from the software RenderManager/tvpgl routines. This
// library is compiled separately: a Layer failure never disables presentation.
static const char* kMetalLayerShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct LayerParameters {
    int4 destination, source, clip;
    int4 operation; // kind, opacity, flags, sampling (0 nearest / 1 linear)
    int4 color;
};
int4 layerBytes(texture2d<float, access::read> texture, int2 coordinate) {
    return int4(round(texture.read(uint2(coordinate)) * 255.0));
}
int4 layerSample(texture2d<float, access::read> texture, constant LayerParameters& p, int2 xy) {
    int2 size = abs(p.source.zw - p.source.xy);
    int2 destinationSize = p.destination.zw - p.destination.xy;
    float2 point = float2(xy - p.destination.xy) * float2(size) / float2(destinationSize);
    int2 base = min(p.source.xy, p.source.zw);
    bool2 reverse = p.source.zw < p.source.xy;
    if (p.operation.w == 0 || all(size == destinationSize)) {
        int2 sample = clamp(int2(point + 0.5), int2(0), size - 1);
        sample = select(sample, size - 1 - sample, reverse);
        return layerBytes(texture, base + sample);
    }
    // Match software ResizeRGBA/SampleBilinear, including its edge
    // extrapolation and byte conversion; hardware linear filtering differs.
    int2 a = max(min(int2(point), size - 2), int2(0));
    int2 b = min(a + 1, size - 1);
    float2 factor = point - float2(a);
    factor = select(factor, float2(0), size == int2(1));
    int2 aa = select(a, size - 1 - a, reverse);
    int2 bb = select(b, size - 1 - b, reverse);
    float4 c00 = float4(layerBytes(texture, base + aa));
    float4 c10 = float4(layerBytes(texture, base + int2(bb.x, aa.y)));
    float4 c01 = float4(layerBytes(texture, base + int2(aa.x, bb.y)));
    float4 c11 = float4(layerBytes(texture, base + bb));
    float4 value = (1-factor.x)*(1-factor.y)*c00 + factor.x*(1-factor.y)*c10 +
                   (1-factor.x)*factor.y*c01 + factor.x*factor.y*c11;
    return int4(max(value + 0.5, float4(0))) & int4(255);
}
// Two-float arithmetic preserves the host double inverse map at half-pixel
// boundaries without requiring unsupported Metal double precision.
struct AffinePair { float high, low; };
AffinePair affineAdd(AffinePair a, AffinePair b) {
    float s=a.high+b.high, v=s-a.high;
    float e=(a.high-(s-v))+(b.high-v)+a.low+b.low;
    float h=s+e;
    return {h,e-(h-s)};
}
AffinePair affineMultiply(float high,float low,float coordinate) {
    float h=high*coordinate;
    return {h,fma(high,coordinate,-h)+low*coordinate};
}
float affineCoordinate(float ah,float al,float bh,float bl,float ch,float cl,float x,float y) {
    AffinePair sum=affineAdd(affineMultiply(ah,al,x),affineMultiply(bh,bl,y));
    sum=affineAdd(sum,{ch,cl});
    return sum.high+sum.low;
}
int affineBilinearByte(float x,float y,int a,int b,int c,int d) {
    float v=(1-x)*(1-y)*float(a)+x*(1-y)*float(b)+(1-x)*y*float(c)+x*y*float(d);
    return int(max(v+0.5f,0.0f)) & 255;
}
struct AffineParameters {
    int4 clip,crop;
    float4 high[2],low[2];
    int4 sampling;
};
kernel void affineCopyLayer(uint2 tid [[thread_position_in_grid]],
                            constant AffineParameters& p [[buffer(0)]],
                            texture2d<float,access::read> source [[texture(0)]],
                            texture2d<float,access::write> target [[texture(1)]]) {
    int2 xy=p.clip.xy+int2(tid);
    if(any(xy>=p.clip.zw)) return;
    float x=float(tid.x)+0.5, y=float(tid.y)+0.5;
    float2 point;
    point.x=affineCoordinate(p.high[0].x,p.low[0].x,p.high[0].y,p.low[0].y,p.high[0].z,p.low[0].z,x,y);
    point.y=affineCoordinate(p.high[1].x,p.low[1].x,p.high[1].y,p.low[1].y,p.high[1].z,p.low[1].z,x,y);
    int2 size=p.crop.zw-p.crop.xy;
    int4 result=int4(0);
    if(all(point>=float2(0.5)) && all(point<float2(size)-0.5)) {
        if(p.sampling.x==0) result=layerBytes(source,p.crop.xy+clamp(int2(point+0.5),int2(0),size-1));
        else {
            int2 a=clamp(int2(point),int2(0),max(size-2,int2(0)));
            int2 b=min(a+1,size-1);
            float2 f=select(point-float2(a),float2(0),size==int2(1));
            int4 c00=layerBytes(source,p.crop.xy+a);
            int4 c10=layerBytes(source,p.crop.xy+int2(b.x,a.y));
            int4 c01=layerBytes(source,p.crop.xy+int2(a.x,b.y));
            int4 c11=layerBytes(source,p.crop.xy+b);
            for(int c=0;c<4;++c) result[c]=affineBilinearByte(f.x,f.y,c00[c],c10[c],c01[c],c11[c]);
        }
    }
    // Software Copy replaces the full clip, including transparent warp border.
    target.write(float4(result)/255.0,uint2(xy));
}
// Affine blends sample the same software warp as Copy, then apply the ordinary
// byte blend independently. A transparent warp border is still a source pixel
// and must participate in the blend over the entire clip.
int4 affineBlendSample(texture2d<float,access::read> source,
                       constant AffineParameters& p,uint2 tid) {
    float x=float(tid.x)+0.5, y=float(tid.y)+0.5;
    float2 point;
    point.x=affineCoordinate(p.high[0].x,p.low[0].x,p.high[0].y,p.low[0].y,p.high[0].z,p.low[0].z,x,y);
    point.y=affineCoordinate(p.high[1].x,p.low[1].x,p.high[1].y,p.low[1].y,p.high[1].z,p.low[1].z,x,y);
    int2 size=p.crop.zw-p.crop.xy;
    int4 result=int4(0);
    if(all(point>=float2(0.5)) && all(point<float2(size)-0.5)) {
        if(p.sampling.x==0) result=layerBytes(source,p.crop.xy+clamp(int2(point+0.5),int2(0),size-1));
        else {
            int2 a=clamp(int2(point),int2(0),max(size-2,int2(0)));
            int2 b=min(a+1,size-1);
            float2 f=select(point-float2(a),float2(0),size==int2(1));
            int4 c00=layerBytes(source,p.crop.xy+a);
            int4 c10=layerBytes(source,p.crop.xy+int2(b.x,a.y));
            int4 c01=layerBytes(source,p.crop.xy+int2(a.x,b.y));
            int4 c11=layerBytes(source,p.crop.xy+b);
            for(int c=0;c<4;++c) result[c]=affineBilinearByte(f.x,f.y,c00[c],c10[c],c01[c],c11[c]);
        }
    }
    return result;
}
struct AffineBlendParameters {
    AffineParameters affine;
    int4 operation; // kind, opacity, flags, reserved
};
// Keep numerator/denominator low parts until after division. Rounding each
// linear expression to float before the ratio changes half-pixel decisions.
AffinePair perspectiveLinearPair(float ah,float al,float bh,float bl,float ch,float cl,float x,float y) {
    AffinePair sum=affineAdd(affineMultiply(ah,al,x),affineMultiply(bh,bl,y));
    return affineAdd(sum,{ch,cl});
}
float perspectiveCoordinate(float ah,float al,float bh,float bl,float ch,float cl,
                            float gh,float gl,float hh,float hl,float ih,float il,float x,float y) {
    AffinePair n=perspectiveLinearPair(ah,al,bh,bl,ch,cl,x,y);
    AffinePair d=perspectiveLinearPair(gh,gl,hh,hl,ih,il,x,y);
    // Compare the compensated value with the software double threshold before
    // rounding it to float; otherwise values just below the pole guard escape.
    AffinePair absolute=d.high<0 ? AffinePair{-d.high,-d.low} : d;
    if(absolute.high<1e-12f || (absolute.high==1e-12f && absolute.low<3.9958028e-21f)) return -1.0f;
    float q=n.high/d.high;
    AffinePair product=affineMultiply(d.high,d.low,q);
    AffinePair remainder=affineAdd(n,{-product.high,-product.low});
    return q+(remainder.high+remainder.low)/d.high;
}
struct PerspectiveParameters {
    LayerParameters rect;
    float4 high[3],low[3];
    int4 shape; // rectangle shortcut, remaining fields reserved
};
int4 perspectiveSample(texture2d<float,access::read> source,
                       constant PerspectiveParameters& p,uint2 tid,int2 xy) {
    if(p.shape.x!=0) return layerSample(source,p.rect,xy);
    float x=float(tid.x)+0.5f,y=float(tid.y)+0.5f;
    float2 point;
    point.x=perspectiveCoordinate(p.high[0].x,p.low[0].x,p.high[0].y,p.low[0].y,p.high[0].z,p.low[0].z,
        p.high[2].x,p.low[2].x,p.high[2].y,p.low[2].y,p.high[2].z,p.low[2].z,x,y);
    point.y=perspectiveCoordinate(p.high[1].x,p.low[1].x,p.high[1].y,p.low[1].y,p.high[1].z,p.low[1].z,
        p.high[2].x,p.low[2].x,p.high[2].y,p.low[2].y,p.high[2].z,p.low[2].z,x,y);
    int2 size=int2(source.get_width(),source.get_height());
    int4 result=int4(0);
    if(all(point>=float2(0.5)) && all(point<float2(size)-0.5)) {
        if(p.rect.operation.w==0) result=layerBytes(source,clamp(int2(point+0.5),int2(0),size-1));
        else {
            int2 a=clamp(int2(point),int2(0),max(size-2,int2(0)));
            int2 b=min(a+1,size-1);
            float2 f=select(point-float2(a),float2(0),size==int2(1));
            int4 c00=layerBytes(source,a),c10=layerBytes(source,int2(b.x,a.y));
            int4 c01=layerBytes(source,int2(a.x,b.y)),c11=layerBytes(source,b);
            for(int c=0;c<4;++c) result[c]=affineBilinearByte(f.x,f.y,c00[c],c10[c],c01[c],c11[c]);
        }
    }
    return result;
}
uint layerPack(int4 c) {
    return (uint(c.r) & 255u) | ((uint(c.g) & 255u) << 8) |
           ((uint(c.b) & 255u) << 16) | ((uint(c.a) & 255u) << 24);
}
int4 layerUnpack(uint c) {
    return int4(int(c & 255u), int((c >> 8) & 255u),
                int((c >> 16) & 255u), int((c >> 24) & 255u));
}
// Packed unsigned formulas preserve tvpgl's byte rounding and wraparound.
// These helpers are also extracted verbatim for portable software parity tests.
uint layerPremulPixel(uint d, uint s, int opa, uint flags) {
    if (opa != 255) {
        s = (((s & 0xff00ffu) * uint(opa) >> 8) & 0xff00ffu) +
            (((s >> 8) & 0xff00ffu) * uint(opa) & 0xff00ff00u);
    }
    uint sa = s >> 24, result = 0;
    for (uint shift = 0; shift < 24; shift += 8) {
        uint color = min(255u, ((s >> shift) & 255u) +
                         ((((d >> shift) & 255u) * (255u - sa)) >> 8));
        result |= color << shift;
    }
    uint da = d >> 24;
    uint alpha = (flags & 1u) != 0 ? da : da + sa - ((da * sa) >> 8);
    if ((flags & 1u) == 0) alpha -= alpha >> 8;
    return result | (alpha << 24);
}
uint layerPsPixel(uint d, uint s, int kind, int opa, uint flags) {
    uint alpha = s >> 24;
    if (opa != 255) alpha = (alpha * uint(opa)) >> 8;
    if (kind == TVP_LAYER_KIND_PsScreen || kind == TVP_LAYER_KIND_PsColorDodge5) {
        uint result=0;
        for(uint shift=0;shift<24;shift+=8) {
            uint dc=(d>>shift)&255u, sc=(s>>shift)&255u, color;
            if(kind==TVP_LAYER_KIND_PsScreen) color=dc+(((sc-((dc*sc)>>8))*alpha)>>8);
            else {
                sc=(sc*alpha)>>8;
                color=255u-sc<=dc ? 255u : dc*255u/(255u-sc);
            }
            result|=color<<shift;
        }
        return result | ((flags & 1u)!=0 ? d & 0xff000000u : 0u);
    }
    if (kind == TVP_LAYER_KIND_PsMul) {
        s = (((((d >> 16) & 255u) * (s & 0x00ff0000u)) & 0xff000000u) |
             ((((d >> 8) & 255u) * (s & 0x0000ff00u)) & 0x00ff0000u) |
             (((d & 255u) * (s & 255u)))) >> 8;
    } else {
        uint blended = 0;
        for (uint shift = 0; shift < 24; shift += 8) {
            uint dc = (d >> shift) & 255u, sc = (s >> shift) & 255u;
            if (kind == TVP_LAYER_KIND_PsHardLight) { uint swap = dc; dc = sc; sc = swap; }
            // Production software enables TVPPS_USE_OVERLAY_TABLE: divide by
            // 255, rather than the approximate >>7 non-table implementation.
            uint product = sc * dc * 2u / 255u;
            uint color = dc < 128u ? product : (sc + dc) * 2u - product - 255u;
            blended |= color << shift;
        }
        s = blended;
    }
    uint rb = d & 0x00ff00ffu, g = d & 0x0000ff00u;
    uint result = (((((s & 0x00ff00ffu) - rb) * alpha) >> 8) + rb) & 0x00ff00ffu;
    result |= (((((s & 0x0000ff00u) - g) * alpha) >> 8) + g) & 0x0000ff00u;
    return result | ((flags & 1u) != 0 ? d & 0xff000000u : 0u);
}
uint layerAlphaToPremulPixel(uint d) {
    uint alpha = d >> 24;
    return (((((d & 0x00ff00u) * alpha) & 0x00ff0000u) +
             (((d & 0xff00ffu) * alpha) & 0xff00ff00u)) >> 8) + (d & 0xff000000u);
}
uint layerAddPixel(uint d,uint s,int opa,uint flags) {
    uint result=0;
    for(uint shift=0;shift<24;shift+=8) {
        uint sc=(s>>shift)&255u;
        if(opa!=255) sc=(sc*uint(opa))>>8;
        result|=min(255u,((d>>shift)&255u)+sc)<<shift;
    }
    uint alpha=d>>24;
    if(opa==255 && (flags&1u)==0) alpha=min(255u,alpha+(s>>24));
    return result | (alpha<<24);
}
uint layerMaskPixel(uint d, uint s, int kind) {
    if (kind == TVP_LAYER_KIND_GrayScale) {
        uint gray = ((s & 255u) * 19u + ((s >> 8) & 255u) * 183u + ((s >> 16) & 255u) * 54u) >> 8;
        return (s & 0xff000000u) | gray * 0x010101u;
    }
    uint alpha = s & 255u;
    if (kind == TVP_LAYER_KIND_MultiplyAlpha) { uint product = (d >> 24) * (s >> 24); alpha = (product + (product >> 7)) >> 8; }
    return (d & 0xffffffu) | (alpha << 24);
}
uint constAlphaSD(uint s1, uint s2, uint opa);
uint univTransBlendARGB(uint s1, uint s2, uint opa);
uint layerP1APixel(uint d,uint s,int kind,int opa,uint flags) {
    if(kind==TVP_LAYER_KIND_AlphaSD) return constAlphaSD(d,s,uint(opa));
    if(kind==TVP_LAYER_KIND_RemoveOpacity) {
        uint alpha=d>>24, mask=s&255u;
        uint result=opa==255 ? (alpha*(255u-mask))>>8 :
            (alpha*(65535u-mask*uint(opa>127 ? opa+1 : opa)))>>16;
        return (d&0xffffffu)|(result<<24);
    }
    uint result=0;
    for(uint shift=0;shift<24;shift+=8) {
        uint dc=(d>>shift)&255u, sc=(s>>shift)&255u, color=0;
        if(kind==TVP_LAYER_KIND_Sub || kind==TVP_LAYER_KIND_Mul) {
            if(opa!=255) sc=255u-(((255u-sc)*uint(opa))>>8);
            color=kind==TVP_LAYER_KIND_Sub ? uint(max(0,int(dc)+int(sc)-255)) : (dc*sc)>>8;
        } else if(kind==TVP_LAYER_KIND_ColorDodge) {
            if(opa!=255) sc=(sc*uint(opa))>>8;
            uint denominator=255u-sc;
            uint reciprocal=denominator==0 ? 65536u : 65536u/denominator;
            color=min(255u,(dc*reciprocal)>>8);
        } else if(kind==TVP_LAYER_KIND_Darken || kind==TVP_LAYER_KIND_Lighten) {
            uint blended=kind==TVP_LAYER_KIND_Darken ? min(dc,sc) : max(dc,sc);
            color=opa==255 ? blended : uint(int(dc)+((int(blended)-int(dc))*opa>>8));
        } else if(kind==TVP_LAYER_KIND_Screen) {
            if(opa!=255) sc=(sc*uint(opa))>>8;
            color=255u-(((255u-dc)*(255u-sc))>>8);
        }
        result|=(color&255u)<<shift;
    }
    // Both current Mul bindings preserve alpha, including the non-HDA name.
    return result|((kind==TVP_LAYER_KIND_Mul || (flags&1u)!=0) ? d&0xff000000u : 0u);
}
uint layerPremulToAlphaPixel(uint s) {
    uint alpha=s>>24, result=s&0xff000000u;
    for(uint shift=0;shift<24;shift+=8) {
        uint color=alpha==0 ? 0u : min(255u,(((s>>shift)&255u)*255u)/alpha);
        result|=color<<shift;
    }
    return result;
}
uint layerGammaPixel(uint d,uint flags,const device uchar* gamma) {
    uint alpha=d>>24, result=d&0xff000000u;
    if((flags&4u)==0 && (flags&16u)==0 && alpha==0) return d;
    if((flags&4u)!=0 && d==0) return 0;
    // tTVPGLGammaAdjustTempData stores B/G/R tables. Software's low byte
    // indexes R and its high RGB byte indexes B, preserving existing ordering.
    for(uint channel=0;channel<3;++channel) {
        uint shift=channel*8u, color=(d>>shift)&255u, offset=(2u-channel)*256u;
        if((flags&4u)==0 || alpha==255) color=uint(gamma[offset+color]);
        else {
            uint adjusted=alpha+(alpha>>7);
            if(color>alpha) color=(uint(gamma[offset+255u])*adjusted>>8)+color-alpha;
            else {
                uint reciprocal=alpha==0 ? 32767u : min(32767u,65536u/alpha);
                uint index=min(255u,(reciprocal*color)>>8);
                color=uint(gamma[offset+index])*adjusted>>8;
            }
        }
        result|=(color&255u)<<shift;
    }
    return result;
}
uint layerPsP1BPixel(uint d,uint s,int kind,int opa,uint flags,const device uchar* psTables) {
    uint alpha=s>>24;
    if(opa!=255) alpha=(alpha*uint(opa))>>8;
    uint result=0;
    for(uint shift=0;shift<24;shift+=8) {
        int dc=int((d>>shift)&255u),sc=int((s>>shift)&255u),blended=sc,color;
        if(kind==TVP_LAYER_KIND_PsAdd) blended=min(255,dc+sc);
        else if(kind==TVP_LAYER_KIND_PsSub) blended=max(0,dc+sc-255);
        else if(kind==TVP_LAYER_KIND_PsLighten) blended=max(dc,sc);
        else if(kind==TVP_LAYER_KIND_PsDarken) blended=min(dc,sc);
        else if(kind==TVP_LAYER_KIND_PsDiff) blended=max(dc,sc)-min(dc,sc);
        else if(kind==TVP_LAYER_KIND_PsSoftLight || kind==TVP_LAYER_KIND_PsColorDodge || kind==TVP_LAYER_KIND_PsColorBurn) {
            uint offset=kind==TVP_LAYER_KIND_PsSoftLight ? 0u :
                        kind==TVP_LAYER_KIND_PsColorDodge ? 65536u : 131072u;
            blended=int(psTables[offset+uint(sc)*256u+uint(dc)]);
        }
        if(kind==TVP_LAYER_KIND_PsDiff5) {
            // Legacy 5.x fades SOURCE first, then takes the absolute difference.
            sc=(sc*int(alpha))>>8;
            color=max(dc,sc)-min(dc,sc);
        } else if(kind==TVP_LAYER_KIND_PsExclusion) {
            // Match the active packed functor's >>7 product, not /255.
            color=dc+(((sc-((dc*sc)>>7))*int(alpha))>>8);
        } else color=dc+(((blended-dc)*int(alpha))>>8);
        result|=(uint(color)&255u)<<shift;
    }
    return result|((flags&1u)!=0 ? d&0xff000000u : 0u);
}
int4 layerPixel(int4 d,int4 s,int4 color,int kind,int opa,int flags,const device uchar* tables,const device uchar* gamma,const device uchar* psTables) {
    bool hold=(flags&1)!=0, straightDestination=(flags&2)!=0;
    bool premultipliedDestination=(flags&4)!=0;
    bool full=opa==255 && (flags&8)!=0;
    int4 result=int4(0);
    switch (kind) {
        case TVP_LAYER_KIND_Copy: result = s; break;
        case TVP_LAYER_KIND_CopyColor: result = int4(s.rgb, d.a); break;
        case TVP_LAYER_KIND_CopyMask: result = int4(d.rgb, s.a); break;
        case TVP_LAYER_KIND_CopyOpaque: result = int4(s.rgb, 255); break;
        case TVP_LAYER_KIND_Fill: result = color; break;
        case TVP_LAYER_KIND_FillColor: result = int4(color.rgb, d.a); break;
        case TVP_LAYER_KIND_FillMask: result = int4(d.rgb, opa); break;
        case TVP_LAYER_KIND_Alpha:
        case TVP_LAYER_KIND_ConstAlpha:
        case TVP_LAYER_KIND_ColorMap:
        case TVP_LAYER_KIND_FillBlend: {
            int alpha;
            if (kind == TVP_LAYER_KIND_ConstAlpha || kind == TVP_LAYER_KIND_FillBlend) alpha = opa;
            else {
                alpha = kind == TVP_LAYER_KIND_ColorMap ? s.r : s.a;
                if (!full) alpha = (alpha * opa) >> 8;
            }
            int3 rgb = kind == TVP_LAYER_KIND_ColorMap || kind == TVP_LAYER_KIND_FillBlend ? color.rgb : s.rgb;
            if (straightDestination) {
                uint index = uint((alpha << 8) + d.a);
                int ratio = int(tables[index]);
                result = int4(d.rgb + (((rgb - d.rgb) * ratio) >> 8), kind == TVP_LAYER_KIND_FillBlend ? 255 - (((255-d.a)*(255-alpha))>>8) : int(tables[65536 + index]));
            } else if (premultipliedDestination) {
                int3 premul = kind == TVP_LAYER_KIND_ConstAlpha ? rgb : (rgb * alpha) >> 8;
                int3 output = min(((d.rgb * (255 - alpha)) >> 8) + premul, int3(255));
                int da = d.a + alpha - ((d.a * alpha) >> 8);
                da -= da >> 8;
                result = int4(output, da);
            } else {
                result = int4(kind == TVP_LAYER_KIND_FillBlend ? ((d.rgb * (255-alpha) + rgb * alpha)>>8) : d.rgb + (((rgb - d.rgb) * alpha) >> 8), hold ? d.a : 0);
            }
            break;
        }
        case TVP_LAYER_KIND_RemoveConstOpacity:
            // TVPRemoveConstOpacity: RGB is preserved exactly; alpha uses the
            // software byte formula A * (255 - strength) >> 8.
            result = int4(d.rgb, (d.a * (255 - opa)) >> 8);
            break;
        case TVP_LAYER_KIND_AdditiveAlpha:
            result = layerUnpack(layerPremulPixel(layerPack(d), layerPack(s), opa, uint(flags)));
            break;
        case TVP_LAYER_KIND_PsMul:
        case TVP_LAYER_KIND_PsOverlay:
        case TVP_LAYER_KIND_PsHardLight:
        case TVP_LAYER_KIND_PsScreen:
        case TVP_LAYER_KIND_PsColorDodge5:
            result = layerUnpack(layerPsPixel(layerPack(d), layerPack(s), kind, opa, uint(flags)));
            break;
        case TVP_LAYER_KIND_PsAlpha:
        case TVP_LAYER_KIND_PsAdd:
        case TVP_LAYER_KIND_PsSub:
        case TVP_LAYER_KIND_PsSoftLight:
        case TVP_LAYER_KIND_PsColorDodge:
        case TVP_LAYER_KIND_PsColorBurn:
        case TVP_LAYER_KIND_PsLighten:
        case TVP_LAYER_KIND_PsDarken:
        case TVP_LAYER_KIND_PsDiff:
        case TVP_LAYER_KIND_PsDiff5:
        case TVP_LAYER_KIND_PsExclusion:
            result=layerUnpack(layerPsP1BPixel(layerPack(d),layerPack(s),kind,opa,uint(flags),psTables));
            break;
        case TVP_LAYER_KIND_Add:
            result = layerUnpack(layerAddPixel(layerPack(d), layerPack(s), opa, uint(flags)));
            break;
        case TVP_LAYER_KIND_Sub:
        case TVP_LAYER_KIND_Mul:
        case TVP_LAYER_KIND_ColorDodge:
        case TVP_LAYER_KIND_Darken:
        case TVP_LAYER_KIND_Lighten:
        case TVP_LAYER_KIND_Screen:
        case TVP_LAYER_KIND_RemoveOpacity:
        case TVP_LAYER_KIND_AlphaSD:
            result=layerUnpack(layerP1APixel(layerPack(d),layerPack(s),kind,opa,uint(flags)));
            break;
        case TVP_LAYER_KIND_AdditiveAlphaToAlpha:
            result=layerUnpack(layerPremulToAlphaPixel(layerPack(s)));
            break;
        case TVP_LAYER_KIND_AdjustGamma:
            result=layerUnpack(layerGammaPixel(layerPack(d),uint(flags),gamma));
            break;
        case TVP_LAYER_KIND_AlphaToAdditiveAlpha:
            result = layerUnpack(layerAlphaToPremulPixel(layerPack(s)));
            break;
        case TVP_LAYER_KIND_GrayScale:
        case TVP_LAYER_KIND_CopyBlueToAlpha:
        case TVP_LAYER_KIND_MultiplyAlpha:
            result = layerUnpack(layerMaskPixel(layerPack(d), layerPack(s), kind));
            break;
    }
    return result & int4(255);
}
kernel void affineBlendLayer(uint2 tid [[thread_position_in_grid]],
                             constant AffineBlendParameters& p [[buffer(0)]],
                             const device uchar* tables [[buffer(1)]],
                             const device uchar* gamma [[buffer(2)]],
                             const device uchar* psTables [[buffer(3)]],
                             texture2d<float,access::read> source [[texture(0)]],
                             texture2d<float,access::read> snapshot [[texture(1)]],
                             texture2d<float,access::write> target [[texture(2)]]) {
    int2 xy=p.affine.clip.xy+int2(tid);
    if(any(xy>=p.affine.clip.zw)) return;
    int4 d=layerBytes(snapshot,int2(tid));
    int4 s=affineBlendSample(source,p.affine,tid);
    int4 result=layerPixel(d,s,int4(0),p.operation.x,p.operation.y,p.operation.z,tables,gamma,psTables);
    target.write(float4(result)/255.0,uint2(xy));
}
kernel void perspectiveLayer(uint2 tid [[thread_position_in_grid]],
                             constant PerspectiveParameters& p [[buffer(0)]],
                             const device uchar* tables [[buffer(1)]],
                             const device uchar* gamma [[buffer(2)]],
                             const device uchar* psTables [[buffer(3)]],
                             texture2d<float,access::read> source [[texture(0)]],
                             texture2d<float,access::read> snapshot [[texture(1)]],
                             texture2d<float,access::write> target [[texture(2)]]) {
    int2 xy=p.rect.clip.xy+int2(tid);
    if(any(xy>=p.rect.clip.zw)) return;
    int4 d=int4(0);
    if(layerReadsTarget(p.rect.operation.x)) d=layerBytes(snapshot,int2(tid));
    int4 s=perspectiveSample(source,p,tid,xy);
    int4 result=layerPixel(d,s,p.rect.color,p.rect.operation.x,p.rect.operation.y,p.rect.operation.z,tables,gamma,psTables);
    target.write(float4(result)/255.0,uint2(xy));
}
kernel void ordinaryLayer(uint2 tid [[thread_position_in_grid]],
                          constant LayerParameters& p [[buffer(0)]],
                          const device uchar* tables [[buffer(1)]],
                          const device uchar* gamma [[buffer(2)]],
                          const device uchar* psTables [[buffer(3)]],
                          texture2d<float, access::read> source [[texture(0)]],
                          texture2d<float, access::read> snapshot [[texture(1)]],
#ifdef TVP_LAYER_IN_PLACE
                          texture2d<float, access::read_write> target [[texture(2)]]) {
#else
                          texture2d<float, access::write> target [[texture(2)]]) {
#endif
    int2 xy = p.clip.xy + int2(tid);
    if (any(xy >= p.clip.zw)) return;
    int kind = p.operation.x, opa = p.operation.y, flags = p.operation.z;
    bool overwrite = !layerReadsTarget(kind);
    int4 d = int4(0), s = int4(0), color = p.color;
    if (!overwrite) {
#ifdef TVP_LAYER_IN_PLACE
        // Each thread snapshots only its own pixel in registers. Source aliases
        // are copied separately before dispatch; no neighboring target is read.
        d = int4(round(target.read(uint2(xy)) * 255.0));
#else
        d = layerBytes(snapshot, xy - p.clip.xy);
#endif
    }
    if (layerNeedsSource(kind))
        s = layerSample(source, p, xy);
    int4 result=layerPixel(d,s,color,kind,opa,flags,tables,gamma,psTables);
    target.write(float4(result) / 255.0,uint2(xy));
}
#ifdef TVP_LAYER_FRAMEBUFFER_FETCH
struct LayerRasterVertex { float4 position [[position]]; };
struct LayerRasterColor { float4 value [[color(0), raster_order_group(0)]]; };
vertex LayerRasterVertex ordinaryLayerVertex(uint id [[vertex_id]],
    constant LayerParameters& p [[buffer(0)]],constant int2& targetSize [[buffer(1)]]) {
    int2 corner=int2(int(id&1),int(id>>1));
    float2 xy=float2(select(p.clip.xy,p.clip.zw,bool2(corner)));
    float2 position=xy/float2(targetSize)*2.0-1.0;
    return {float4(position.x,-position.y,0,1)};
}
fragment LayerRasterColor ordinaryLayerFragment(LayerRasterVertex in [[stage_in]],
    float4 previous [[color(0), raster_order_group(0)]],
    constant LayerParameters& p [[buffer(0)]],const device uchar* tables [[buffer(1)]],
    const device uchar* gamma [[buffer(2)]],
    const device uchar* psTables [[buffer(3)]],
    texture2d<float,access::read> source [[texture(0)]]) {
    int2 xy=int2(in.position.xy);
    int kind=p.operation.x;
    int4 s=int4(0);
    if(layerNeedsSource(kind)) s=layerSample(source,p,xy);
    int4 d=int4(round(previous*255.0));
    return {float4(layerPixel(d,s,p.color,kind,p.operation.y,p.operation.z,tables,gamma,psTables))/255.0};
}
#endif


// The active software BoxFilterRGBA averages each byte with integer division.
// Sliding sums preserve that result without CPU readback or radius-dependent
// per-pixel texture reads. Both software blur method names use this formula.
struct BoxBlurParameters { int4 source, destination, radius; };
kernel void boxBlurRows(uint y [[thread_position_in_grid]],
                       constant BoxBlurParameters& p [[buffer(0)]],
                       device uint4* sums [[buffer(1)]],
                       texture2d<float, access::read> source [[texture(0)]]) {
    int w=p.source.z-p.source.x, h=p.source.w-p.source.y;
    if(y>=uint(h)) return;
    int radius=min(p.radius.x,w);
    uint4 sum=uint4(0);
    for(int x=0;x<min(w,radius+1);++x)
        sum+=uint4(layerBytes(source,int2(p.source.x+x,p.source.y+int(y))));
    for(int x=0;x<w;++x) {
        sums[y*uint(w)+uint(x)]=sum;
        if(x-radius>=0) sum-=uint4(layerBytes(source,int2(p.source.x+x-radius,p.source.y+int(y))));
        if(x+radius+1<w) sum+=uint4(layerBytes(source,int2(p.source.x+x+radius+1,p.source.y+int(y))));
    }
}
kernel void boxBlurColumns(uint x [[thread_position_in_grid]],
                          constant BoxBlurParameters& p [[buffer(0)]],
                          const device uint4* sums [[buffer(1)]],
                          texture2d<float, access::write> target [[texture(0)]]) {
    int w=p.source.z-p.source.x, h=p.source.w-p.source.y;
    if(x>=uint(w)) return;
    int rx=min(p.radius.x,w), ry=min(p.radius.y,h);
    uint4 sum=uint4(0);
    for(int y=0;y<min(h,ry+1);++y) sum+=sums[uint(y*w)+x];
    uint columns=uint(min(w,int(x)+rx+1)-max(0,int(x)-rx));
    for(int y=0;y<h;++y) {
        uint area=columns*uint(min(h,y+ry+1)-max(0,y-ry));
        target.write(float4(sum/area)/255.0,uint2(p.destination.xy+int2(int(x),y)));
        if(y-ry>=0) sum-=sums[uint((y-ry)*w)+x];
        if(y+ry+1<h) sum+=sums[uint((y+ry+1)*w)+x];
    }
}

struct DualLayerParameters {
    int4 destination, source1, source2, clip;
    int4 operation; // kind, opacity, flags, reserved
};
uint constAlphaSD(uint s1, uint s2, uint opa) {
    uint rb = s1 & 0x00ff00ffu;
    rb = (rb + ((((s2 & 0x00ff00ffu) - rb) * opa) >> 8)) & 0x00ff00ffu;
    uint g1 = s1 & 0x0000ff00u;
    uint g2 = s2 & 0x0000ff00u;
    return rb | ((g1 + (((g2 - g1) * opa) >> 8)) & 0x0000ff00u);
}
uint constAlphaSDDestAlpha(uint s1, uint s2, uint opacity, const device uchar* tables) {
    uint opa = opacity;
    if (opa > 127u) ++opa; // exact tvpgl rounding adjustment
    uint iopa = 256u - opa;
    uint a1 = s1 >> 24, a2 = s2 >> 24;
    uint addr = ((a2 * opa) & 0xff00u) + ((a1 * iopa) >> 8);
    uint alpha = uint(tables[addr]);

    uint rb = s1 & 0x00ff00ffu;
    rb = (rb + ((((s2 & 0x00ff00ffu) - rb) * alpha) >> 8)) & 0x00ff00ffu;
    uint g1 = s1 & 0x0000ff00u;
    uint g2 = s2 & 0x0000ff00u;
    uint out = rb | ((g1 + (((g2 - g1) * alpha) >> 8)) & 0x0000ff00u);
    out |= (a1 + (((a2 - a1) * opa) >> 8)) << 24;
    return out;
}
kernel void dualSourceLayer(uint2 tid [[thread_position_in_grid]],
                            constant DualLayerParameters& p [[buffer(0)]],
                            const device uchar* tables [[buffer(1)]],
                            texture2d<float, access::read> source1 [[texture(0)]],
                            texture2d<float, access::read> source2 [[texture(1)]],
                            texture2d<float, access::write> target [[texture(2)]]) {
    int2 xy = p.clip.xy + int2(tid);
    if (any(xy >= p.clip.zw)) return;
    int2 offset = xy - p.destination.xy;
    int2 p1 = p.source1.xy + offset;
    int2 p2 = p.source2.xy + offset;
    uint s1 = layerPack(layerBytes(source1, p1));
    uint s2 = layerPack(layerBytes(source2, p2));
    uint out = (p.operation.z & 2) != 0
        ? constAlphaSDDestAlpha(s1, s2, uint(p.operation.y), tables)
        : (p.operation.z & 4) != 0 ? univTransBlendARGB(s1,s2,uint(p.operation.y))
        : constAlphaSD(s1, s2, uint(p.operation.y));
    target.write(float4(layerUnpack(out)) / 255.0, uint2(xy));
}

// UnivTrans integer helpers are also compiled by the portable parity test.
// Unlike ConstAlphaSD_d, the universal blend does NOT adjust weights > 127.
uint univTransBlendARGB(uint s1, uint s2, uint opa) {
    uint rb = s1 & 0x00ff00ffu;
    uint out = (rb + ((((s2 & 0x00ff00ffu) - rb) * opa) >> 8)) & 0x00ff00ffu;
    uint ga = (s1 & 0xff00ff00u) >> 8;
    return out + (((ga + (((((s2 & 0xff00ff00u) >> 8) - ga) * opa) >> 8)) << 8) & 0xff00ff00u);
}
uint univTransPixel(uint s1, uint s2, int rule, int phase, int vague,
                    uint flags, const device uchar* tables) {
    int lower = phase - vague;
    // The switch routines copy the ENTIRE source pixel, including alpha.
    // For vague >= 512 the full-table routines blend even at weight 0/255.
    if (vague < 512) {
        if (rule >= phase) return s1;
        if (rule < lower) return s2;
    }
    uint opa = rule < lower ? 255u : rule >= phase ? 0u :
        uint(255 - ((rule - lower) * 255 / max(vague, 1)));
    if ((flags & 2u) != 0u) {
        uint a1 = s1 >> 24, a2 = s2 >> 24;
        uint addr = ((a2 * opa) & 0xff00u) + ((a1 * (256u - opa)) >> 8);
        // tvpgl's switch_d and full-table _d use different alpha formulas.
        uint alpha = vague < 512 ? uint(tables[65536u + addr]) :
            a1 + (((a2 - a1) * opa) >> 8);
        return constAlphaSD(s1, s2, uint(tables[addr])) |
               (alpha << 24);
    }
    if ((flags & 4u) != 0u) return univTransBlendARGB(s1, s2, opa);
    return constAlphaSD(s1, s2, opa);
}
// End UnivTrans integer helpers.

struct TripleLayerParameters {
    int4 destination, source1, source2, rule, clip;
    int4 operation; // kind, flags, phase, vague
};
kernel void univTransLayer(uint2 tid [[thread_position_in_grid]],
                          constant TripleLayerParameters& p [[buffer(0)]],
                          const device uchar* tables [[buffer(1)]],
                          texture2d<float, access::read> source1 [[texture(0)]],
                          texture2d<float, access::read> source2 [[texture(1)]],
                          texture2d<float, access::read> rule [[texture(2)]],
                          texture2d<float, access::write> target [[texture(3)]]) {
    int2 xy = p.clip.xy + int2(tid);
    if (any(xy >= p.clip.zw)) return;
    int2 offset = xy - p.destination.xy;
    uint s1 = layerPack(layerBytes(source1, p.source1.xy + offset));
    uint s2 = layerPack(layerBytes(source2, p.source2.xy + offset));
    int weight = layerBytes(rule, p.rule.xy + offset).r;
    uint out = univTransPixel(s1, s2, weight, p.operation.z, p.operation.w,
                              uint(p.operation.y), tables);
    target.write(float4(layerUnpack(out)) / 255.0, uint2(xy));
}
)MSL";

inline std::string TVPBuildMetalLayerShaderSource(const char* options = "") {
    return std::string(options) + TVP_LAYER_OPERATION_MSL_DEFINITIONS + kMetalLayerShaders;
}
