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
    bool overwrite = kind == 1 || kind == 4 || kind == 5;
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
    if (kind < 5 || (kind >= 8 && kind <= 10)) s = layerSample(source, p, xy);
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
    }
    target.write(float4(result & int4(255)) / 255.0, uint2(xy));
}

struct DualLayerParameters {
    int4 destination, source1, source2, clip;
    int4 operation; // kind, opacity, flags, reserved
};
uint layerPack(int4 c) {
    return (uint(c.r) & 255u) | ((uint(c.g) & 255u) << 8) |
           ((uint(c.b) & 255u) << 16) | ((uint(c.a) & 255u) << 24);
}
int4 layerUnpack(uint c) {
    return int4(int(c & 255u), int((c >> 8) & 255u),
                int((c >> 16) & 255u), int((c >> 24) & 255u));
}
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
)MSL";
