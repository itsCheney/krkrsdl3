#pragma once

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
    if (kind == 16) {
        s = (((((d >> 16) & 255u) * (s & 0x00ff0000u)) & 0xff000000u) |
             ((((d >> 8) & 255u) * (s & 0x0000ff00u)) & 0x00ff0000u) |
             (((d & 255u) * (s & 255u)))) >> 8;
    } else {
        uint blended = 0;
        for (uint shift = 0; shift < 24; shift += 8) {
            uint dc = (d >> shift) & 255u, sc = (s >> shift) & 255u;
            if (kind == 18) { uint swap = dc; dc = sc; sc = swap; }
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
uint layerMaskPixel(uint d, uint s, int kind) {
    if (kind == 20) {
        uint gray = ((s & 255u) * 19u + ((s >> 8) & 255u) * 183u + ((s >> 16) & 255u) * 54u) >> 8;
        return (s & 0xff000000u) | gray * 0x010101u;
    }
    uint alpha = s & 255u;
    if (kind == 22) { uint product = (d >> 24) * (s >> 24); alpha = (product + (product >> 7)) >> 8; }
    return (d & 0xffffffu) | (alpha << 24);
}
kernel void ordinaryLayer(uint2 tid [[thread_position_in_grid]],
                          constant LayerParameters& p [[buffer(0)]],
                          const device uchar* tables [[buffer(1)]],
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
    bool hold = (flags & 1) != 0, straightDestination = (flags & 2) != 0;
    bool premultipliedDestination = (flags & 4) != 0;
    bool full = opa == 255 && (flags & 8) != 0;
    bool overwrite = kind == 1 || kind == 4 || kind == 5 || kind == 19 || kind == 20;
    int4 d = int4(0), s = int4(0), color = p.color, result = int4(0);
    if (!overwrite) {
#ifdef TVP_LAYER_IN_PLACE
        // Each thread snapshots only its own pixel in registers. Source aliases
        // are copied separately before dispatch; no neighboring target is read.
        d = int4(round(target.read(uint2(xy)) * 255.0));
#else
        d = layerBytes(snapshot, xy - p.clip.xy);
#endif
    }
    if (kind < 5 || (kind >= 8 && kind <= 10) || (kind >= 15 && kind <= 22))
        s = layerSample(source, p, xy);
    switch (kind) {
        case 1: result = s; break;
        case 2: result = int4(s.rgb, d.a); break;
        case 3: result = int4(d.rgb, s.a); break;
        case 4: result = int4(s.rgb, 255); break;
        case 5: result = color; break;
        case 6: result = int4(color.rgb, d.a); break;
        case 7: result = int4(d.rgb, opa); break;
        case 8:
        case 9:
        case 10:
        case 11: {
            int alpha;
            if (kind == 9 || kind == 11) alpha = opa;
            else {
                alpha = kind == 10 ? s.r : s.a;
                if (!full) alpha = (alpha * opa) >> 8;
            }
            int3 rgb = kind == 10 || kind == 11 ? color.rgb : s.rgb;
            if (straightDestination) {
                uint index = uint((alpha << 8) + d.a);
                int ratio = int(tables[index]);
                result = int4(d.rgb + (((rgb - d.rgb) * ratio) >> 8), kind == 11 ? 255 - (((255-d.a)*(255-alpha))>>8) : int(tables[65536 + index]));
            } else if (premultipliedDestination) {
                int3 premul = kind == 9 ? rgb : (rgb * alpha) >> 8;
                int3 output = min(((d.rgb * (255 - alpha)) >> 8) + premul, int3(255));
                int da = d.a + alpha - ((d.a * alpha) >> 8);
                da -= da >> 8;
                result = int4(output, da);
            } else {
                result = int4(kind == 11 ? ((d.rgb * (255-alpha) + rgb * alpha)>>8) : d.rgb + (((rgb - d.rgb) * alpha) >> 8), hold ? d.a : 0);
            }
            break;
        }
        case 12:
            // TVPRemoveConstOpacity: RGB is preserved exactly; alpha uses the
            // software byte formula A * (255 - strength) >> 8.
            result = int4(d.rgb, (d.a * (255 - opa)) >> 8);
            break;
        case 15:
            result = layerUnpack(layerPremulPixel(layerPack(d), layerPack(s), opa, uint(flags)));
            break;
        case 16:
        case 17:
        case 18:
            result = layerUnpack(layerPsPixel(layerPack(d), layerPack(s), kind, opa, uint(flags)));
            break;
        case 19:
            result = layerUnpack(layerAlphaToPremulPixel(layerPack(s)));
            break;
        case 20:
        case 21:
        case 22:
            result = layerUnpack(layerMaskPixel(layerPack(d), layerPack(s), kind));
            break;
    }
    target.write(float4(result & int4(255)) / 255.0, uint2(xy));
}

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
