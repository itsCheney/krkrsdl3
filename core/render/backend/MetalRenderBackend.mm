#include "MetalRenderBackend.h"
#include "MetalLayerShaders.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <simd/simd.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace krkrsdl3
{
namespace
{
// RGBA throughout the backend; only the drawable uses BGRA. Metal performs that
// attachment conversion. Content row zero is the top row, including readback.
const char* kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Vertex { float2 xy; float2 uv; };
struct Varying { float4 position [[position]]; float2 uv; };
struct DeformSurface {
    float4x4 matrix;
    float4 geometry;
    int4 flags;
    float2 control[16];
};
struct Params { float4 color, modulation, rect, uv; int4 flags; float4 values; };
vertex Varying vertexMain(uint i [[vertex_id]], const device Vertex* v [[buffer(0)]]) {
    return {float4(v[i].xy.x, -v[i].xy.y, 0, 1), v[i].uv};
}
float4 bezierBasis(float t) {
    float omt = 1.0 - t;
    return float4(omt * omt * omt,
                  3.0 * t * omt * omt,
                  3.0 * t * t * omt,
                  t * t * t);
}
float2 evalBezier(const device DeformSurface& surface, float u, float v) {
    float4 bu = bezierBasis(u);
    float4 bv = bezierBasis(v);
    float2 result = float2(0.0);
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            result += surface.control[row * 4 + col] * bu[row] * bv[col];
    return result;
}
vertex Varying deformVertexMain(uint i [[vertex_id]],
                                const device float2* gridUV [[buffer(0)]],
                                const device DeformSurface* surfaces [[buffer(1)]],
                                constant uint& surfaceCount [[buffer(2)]]) {
    float u = gridUV[i].x, v = gridUV[i].y;
    float lastX = 0.0, lastY = 0.0;
    float4 trans = float4(1.0);
    for (int n = int(surfaceCount) - 1; n >= 0; --n) {
        const device DeformSurface& surface = surfaces[n];
        if (surface.flags.x == 3) {
            trans = surface.matrix * trans;
            continue;
        }
        if (n < int(surfaceCount) - 1) {
            lastX = (trans.y + surface.geometry.y) / surface.geometry.w;
            lastY = (trans.x + surface.geometry.x) / surface.geometry.z;
        } else {
            lastX = u;
            lastY = v;
        }
        float2 p = surface.flags.x == 1 ? evalBezier(surface, lastX, lastY)
                                        : float2(lastY, lastX);
        trans = surface.matrix * float4(p, 0.0, 1.0);
    }
    // CPU geometry stores y=-trans.y and vertexMain flips it again for Metal.
    // Emit the final Metal clip-space value directly here.
    return {float4(trans.x, trans.y, 0.0, 1.0), float2(v, u)};
}
fragment float4 windowMain(Varying v [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    // The native presentation layer is opaque and copies RGB even when KRKR
    // pixels have zero alpha. Screenshots must reproduce that visible RGB.
    return float4(src.sample(s, v.uv).rgb, 1.0);
}
fragment float4 meshMain(Varying v [[stage_in]], constant Params& p [[buffer(0)]],
                        texture2d<float> src [[texture(0)]], texture2d<float> mask [[texture(1)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    if (p.flags.y && mask.sample(s, v.position.xy / p.values.yz).a < 0.5)
        discard_fragment();
    float4 c = src.sample(s, v.uv);
    if (p.flags.z) c = float4(p.color.rgb, p.color.a * c.a);
    c *= p.modulation;
    c.a *= p.values.x;
    return c;
}
// Layer formulas use byte arithmetic, rather than approximate fixed-function
// alpha blending (which changes source alpha and signed >>8 rounding).
kernel void layerMain(uint2 tid [[thread_position_in_grid]], constant Params& p [[buffer(0)]],
                      texture2d<float, access::read> src [[texture(0)]],
                      texture2d<float, access::read> dst [[texture(1)]],
                      texture2d<float, access::write> out [[texture(2)]]) {
    int2 origin = max(int2(floor(p.rect.xy)), int2(0));
    int2 end = min(int2(ceil(p.rect.xy + p.rect.zw)), int2(out.get_width(), out.get_height()));
    int2 xy = origin + int2(tid);
    if (any(xy >= end)) return;
    float2 uv = p.uv.xy + (p.uv.zw - p.uv.xy) * ((float2(xy) - p.rect.xy + 0.5) / p.rect.zw);
    int2 sz = int2(src.get_width(), src.get_height());
    int2 sampleXY = clamp(int2(uv * float2(sz - 1) + 0.5), int2(0), sz - 1);
    int4 s = int4(round(src.read(uint2(sampleXY)) * 255.0));
    int4 d = int4(round(dst.read(uint2(xy)) * 255.0));
    int4 c = s;
    int opa = p.flags.w;
    switch (p.flags.z) {
        case 1: c = d + (((s - d) * ((s.a * opa) >> 8)) >> 8); break;
        case 2: c = d + (((s - d) * opa) >> 8); break;
        case 3: c = int4(min(d.rgb + ((s.rgb * opa) >> 8), int3(255)), d.a); break;
        case 4: c = max(d - int4(255 - (((255 - s.rgb) * opa) >> 8), s.a), int4(0)); break;
        case 5: c = int4((d.rgb * (255 - (((255 - s.rgb) * opa) >> 8))) >> 8, 0); break;
        case 6: c = int4((d.rgb * (255 - (((255 - s.rgb) * opa) >> 8))) >> 8, d.a); break;
        case 7: c = int4(round(p.color * 255.0)); break;
        case 8: c = int4(s.rgb, d.a); break;
        case 9: c = int4(s.rgb, 255); break;
        case 10: c = int4(d.rgb, s.a); break;
    }
    out.write(float4(clamp(c, int4(0), int4(255))) / 255.0, uint2(xy));
}
)MSL";

struct Params
{
    simd_float4 color = {0, 0, 0, 0};
    simd_float4 modulation = {1, 1, 1, 1};
    simd_float4 rect = {0, 0, 0, 0};
    simd_float4 uv = {0, 0, 1, 1};
    simd_int4 flags = {0, 0, 0, 255};
    simd_float4 values = {1, 0, 0, 0};
};
static_assert(sizeof(Params) == 96, "MSL uniform layout");

bool ValidSize(int w, int h)
{
    return w > 0 && h > 0 && w <= std::numeric_limits<int>::max() / 4 &&
           static_cast<size_t>(h) <= std::numeric_limits<size_t>::max() / (static_cast<size_t>(w) * 4);
}
simd_float4 Color(const float* c)
{
    return c ? simd_make_float4(c[0], c[1], c[2], c[3]) : simd_make_float4(0, 0, 0, 0);
}
} // namespace

struct MetalRenderBackend::Impl
{
    struct Resource
    {
        id<MTLTexture> texture = nil;
        int width = 0, height = 0;
        int bytesPerPixel = 4;
        bool target = false;
        std::vector<uint8_t> readback;
    };
    struct WindowDraw
    {
        id<MTLTexture> texture = nil;
        float x, y, width, height;
    };
    SDL_Window* window = nullptr;
    SDL_MetalView view = nullptr;
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLCommandBuffer> commands = nil, lastSubmitted = nil;
    id<MTLRenderPipelineState> windowPipeline = nil, capturePipeline = nil;
    id<MTLRenderPipelineState> meshPipelines[3] = {nil, nil, nil};
    id<MTLRenderPipelineState> deformPipelines[3] = {nil, nil, nil};
    struct DeformTopology {
        id<MTLBuffer> uv = nil;
        id<MTLBuffer> indices = nil;
        int vertexCount = 0;
        int indexCount = 0;
    };
    std::unordered_map<uint32_t, DeformTopology> deformTopologies;
    id<MTLComputePipelineState> layerPipeline = nil, ordinaryLayerPipeline = nil, ordinaryInPlacePipeline = nil;
    id<MTLComputePipelineState> dualSourceLayerPipeline = nil;
    id<MTLComputeCommandEncoder> ordinaryEncoder = nil;
    id<MTLTexture> ordinaryBoundSource = nil, ordinaryBoundTarget = nil;
    id<MTLBuffer> alphaTables = nil;
    id<MTLTexture> ordinaryDummy = nil, ordinarySnapshot = nil, ordinarySourceSnapshot = nil;
    id<MTLTexture> dualSourceSnapshot1 = nil, dualSourceSnapshot2 = nil;
    id<MTLTexture> destinationSnapshot = nil;
    dispatch_semaphore_t inFlight = dispatch_semaphore_create(2);
    std::shared_ptr<std::atomic<bool>> gpuFailed = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<double>> gpuTimeMS = std::make_shared<std::atomic<double>>(-1.0);
    std::unordered_map<void*, std::unique_ptr<Resource>> resources;
    std::vector<WindowDraw> windows;
    Resource* current = nullptr;
    Resource* mask = nullptr;
    Params mesh, layerParams;
    int width = 0, height = 0;
    size_t transientBytes = 0;
    double pendingQueueWaitMS = 0, lastPresentationWaitMS = -1;
    static constexpr size_t kSubmissionBudget = 16 * 1024 * 1024;
    // Staging buffers are recycled instead of reallocated: uploads and readbacks
    // churn hundreds of MB/s through here. A buffer stays checked out until the
    // command buffer referencing it completes, so reuse is driven by Submit.
    std::vector<id<MTLBuffer>> stagingPool;
    // Buffers handed to command buffers that have not completed yet. Ownership
    // returns to the pool once the referencing command buffer reports done;
    // all pool mutation stays on the render thread.
    std::vector<std::pair<id<MTLCommandBuffer>, std::vector<id<MTLBuffer>>>> stagingInFlight;
    // Checked out for the command buffer currently being encoded. If encoding
    // fails these stay here and attach to the next submission instead, so they
    // are never handed out while an open command buffer still references them.
    std::vector<id<MTLBuffer>> stagingPending;
    static constexpr size_t kStagingPoolBytes = 64 * 1024 * 1024;
    size_t stagingPoolBytes = 0;

    // High-frequency mesh uploads use persistent command-buffer-scoped arenas
    // instead of allocating one MTLBuffer per vertex/index payload.
    struct RingPage {
        id<MTLBuffer> buffer = nil;
        size_t offset = 0;
        id<MTLCommandBuffer> owner = nil;
    };
    struct RingSlice {
        id<MTLBuffer> buffer = nil;
        size_t offset = 0;
        size_t length = 0;
    };
    static constexpr size_t kRingPageBytes = 4 * 1024 * 1024;
    static constexpr size_t kRingAlignment = 256;
    std::array<RingPage, 3> ringPages;
    int currentRingPage = -1;
    static size_t AlignRing(size_t value) {
        return (value + kRingAlignment - 1) & ~(kRingAlignment - 1);
    }
    static bool Retired(id<MTLCommandBuffer> buffer)
    {
        MTLCommandBufferStatus status = buffer.status;
        return status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError;
    }
    void ReturnToPool(id<MTLBuffer> buffer)
    {
        if (!buffer || stagingPoolBytes + buffer.length > kStagingPoolBytes) return;
        stagingPoolBytes += buffer.length;
        stagingPool.push_back(buffer);
    }
    void DrainStaging()
    {
        for (auto it = stagingInFlight.begin(); it != stagingInFlight.end();) {
            if (!Retired(it->first)) { ++it; continue; }
            for (id<MTLBuffer> buffer : it->second) ReturnToPool(buffer);
            it = stagingInFlight.erase(it);
        }
    }
    id<MTLBuffer> AcquireStaging(size_t length)
    {
        if (!length) return nil;
        DrainStaging();
        // Reuse the smallest buffer that fits so a single oversized request
        // cannot starve the common small-upload path.
        size_t best = stagingPool.size();
        for (size_t i = 0; i < stagingPool.size(); ++i)
            if (stagingPool[i].length >= length &&
                (best == stagingPool.size() || stagingPool[i].length < stagingPool[best].length))
                best = i;
        id<MTLBuffer> buffer = nil;
        if (best != stagingPool.size()) {
            buffer = stagingPool[best];
            stagingPoolBytes -= buffer.length;
            stagingPool.erase(stagingPool.begin() + best);
        } else {
            buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
            if (!buffer) return nil;
        }
        stagingPending.push_back(buffer);
        return buffer;
    }

    ~Impl()
    {
        @autoreleasepool {
            Submit(true);
            windows.clear();
            resources.clear();
            destinationSnapshot = nil;
            layer = nil;
            if (view) SDL_Metal_DestroyView(view);
        }
    }
    Resource* Find(void* handle) const
    {
        auto it = resources.find(handle);
        return it == resources.end() ? nullptr : it->second.get();
    }
    id<MTLCommandBuffer> Commands()
    {
        if (gpuFailed->load(std::memory_order_relaxed))
            throw std::runtime_error("Metal GPU command execution failed (see render log)");
        if (!commands) {
            const Uint64 waitStarted = SDL_GetTicksNS();
            dispatch_semaphore_wait(inFlight, DISPATCH_TIME_FOREVER);
            const Uint64 queueWaitNS = SDL_GetTicksNS() - waitStarted;
            pendingQueueWaitMS += static_cast<double>(queueWaitNS) / 1000000.0;
            TVPRecordMetalQueueWait(queueWaitNS);
            commands = [queue commandBuffer];
            if (!commands) {
                dispatch_semaphore_signal(inFlight);
                throw std::runtime_error("Metal command buffer allocation failed");
            }
            // Capture shared completion state, never this (the callback can
            // outlive the C++ resource wrappers).
            dispatch_semaphore_t semaphore = inFlight;
            auto fault = gpuFailed;
            auto timing = gpuTimeMS;
            [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                if (buffer.status == MTLCommandBufferStatusError) {
                    fault->store(true, std::memory_order_relaxed);
                    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Metal GPU error: %s", buffer.error.localizedDescription.UTF8String);
                }
                if (buffer.status == MTLCommandBufferStatusCompleted && buffer.GPUStartTime > 0 &&
                    buffer.GPUEndTime >= buffer.GPUStartTime)
                    timing->store((buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0, std::memory_order_relaxed);
                dispatch_semaphore_signal(semaphore);
            }];
        }
        return commands;
    }
    RingPage& AcquireRingPage()
    {
        Commands(); // Reserve an in-flight slot before binding arena lifetime.
        if (currentRingPage >= 0) return ringPages[static_cast<size_t>(currentRingPage)];

        for (size_t i = 0; i < ringPages.size(); ++i) {
            auto& page = ringPages[i];
            if (page.owner && Retired(page.owner)) {
                page.owner = nil;
                page.offset = 0;
            }
            if (!page.owner) {
                if (!page.buffer) {
                    page.buffer = [device newBufferWithLength:kRingPageBytes
                                                    options:MTLResourceStorageModeShared];
                    if (!page.buffer)
                        throw std::runtime_error("Metal transient ring allocation failed");
                }
                currentRingPage = static_cast<int>(i);
                return page;
            }
        }

        // With three pages and at most two in-flight command buffers this should
        // be rare, but keep a measured safety path instead of allocating forever.
        auto& page = ringPages[0];
        const Uint64 stallStarted = SDL_GetTicksNS();
        [page.owner waitUntilCompleted];
        TVPRecordMetalRingStall(SDL_GetTicksNS() - stallStarted);
        page.owner = nil;
        page.offset = 0;
        currentRingPage = 0;
        return page;
    }
    RingSlice AcquireRing(size_t length)
    {
        if (!length) return {};
        const Uint64 started = SDL_GetTicksNS();
        if (length > kRingPageBytes) {
            id<MTLBuffer> fallback = AcquireStaging(length);
            if (!fallback) return {};
            TVPRecordMetalRingFallback(length);
            return {fallback, 0, length};
        }

        auto* page = &AcquireRingPage();
        size_t offset = AlignRing(page->offset);
        if (offset + length > kRingPageBytes) {
            TVPRecordMetalRingWrap();
            Submit();
            page = &AcquireRingPage();
            offset = AlignRing(page->offset);
        }
        page->offset = offset + length;
        TVPRecordMetalRingSuballoc(length, page->offset, SDL_GetTicksNS() - started);
        return {page->buffer, offset, length};
    }
    void EndOrdinary() {
        if(ordinaryEncoder) { [ordinaryEncoder endEncoding]; ordinaryEncoder=nil; }
        ordinaryBoundSource=nil; ordinaryBoundTarget=nil;
    }
    id<MTLBlitCommandEncoder> Blit() { EndOrdinary(); return [Commands() blitCommandEncoder]; }
    id<MTLComputeCommandEncoder> Compute() { EndOrdinary(); return [Commands() computeCommandEncoder]; }
    id<MTLComputeCommandEncoder> OrdinaryCompute() {
        if(!ordinaryEncoder) {
            // Serial dispatches provide write/read ordering for tracked textures.
            ordinaryEncoder=[Commands() computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
            [ordinaryEncoder setComputePipelineState:ordinaryInPlacePipeline];
            [ordinaryEncoder setBuffer:alphaTables offset:0 atIndex:1];
        }
        return ordinaryEncoder;
    }
    bool Submit(bool wait = false)
    {
        EndOrdinary();
        if (commands) {
            lastSubmitted = commands;
            if (currentRingPage >= 0) {
                auto& page = ringPages[static_cast<size_t>(currentRingPage)];
                page.owner = commands;
                currentRingPage = -1;
            }
            // Staging written into this batch stays checked out until it completes.
            if (!stagingPending.empty())
                stagingInFlight.emplace_back(commands, std::move(stagingPending));
            stagingPending.clear();
            [commands commit];
            TVPRecordMetalSubmit();
            commands = nil;
            transientBytes = 0;
        }
        if (wait && lastSubmitted) {
            const Uint64 waitStarted = SDL_GetTicksNS();
            [lastSubmitted waitUntilCompleted];
            TVPRecordMetalSyncWait(SDL_GetTicksNS() - waitStarted);
            DrainStaging();
            return lastSubmitted.status == MTLCommandBufferStatusCompleted;
        }
        return true;
    }
    id<MTLTexture> Texture(int w, int h, MTLPixelFormat format = MTLPixelFormatRGBA8Unorm)
    {
        if (!ValidSize(w, h)) return nil;
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                   width:w height:h mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        return [device newTextureWithDescriptor:d];
    }
    id<MTLRenderCommandEncoder> Pass(id<MTLTexture> texture, bool clear)
    {
        EndOrdinary();
        MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = texture;
        p.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> encoder = [Commands() renderCommandEncoderWithDescriptor:p];
        if (!encoder) throw std::runtime_error("Metal render encoder allocation failed");
        return encoder;
    }
    void* Create(int w, int h, bool target)
    {
        id<MTLTexture> texture = Texture(w, h);
        if (!texture) return nullptr;
        auto r = std::make_unique<Resource>();
        r->texture = texture;
        r->width = w; r->height = h; r->target = target;
        [Pass(texture, true) endEncoding];
        void* handle = r.get();
        resources.emplace(handle, std::move(r));
        return handle;
    }
    void Destroy(void* handle, bool target)
    {
        Resource* r = Find(handle);
        if (!r || r->target != target) return;
        if (current == r) current = nullptr;
        if (mask == r) mask = nullptr;
        // Command buffers retain encoded Metal resources until GPU completion.
        windows.erase(std::remove_if(windows.begin(), windows.end(),
                      [&](const WindowDraw& draw) { return draw.texture == r->texture; }), windows.end());
        resources.erase(handle);
    }
    void Upload(Resource* r, const uint8_t* pixels, int w, int h, int pitch)
    {
        if (!r || !pixels || !ValidSize(w, h) || w > r->width || h > r->height || pitch < w * 4) return;
        const size_t rowBytes = (static_cast<size_t>(w) * 4 + 255) & ~size_t(255);
        if (static_cast<size_t>(h) > std::numeric_limits<size_t>::max() / rowBytes) return;
        id<MTLBuffer> staging = AcquireStaging(rowBytes * h);
        if (!staging) throw std::runtime_error("Metal upload buffer allocation failed");
        for (int y = 0; y < h; ++y)
            std::memcpy(static_cast<uint8_t*>(staging.contents) + y * rowBytes,
                        pixels + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * 4);
        id<MTLBlitCommandEncoder> e = Blit();
        if (!e) throw std::runtime_error("Metal blit encoder allocation failed");
        [e copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:rowBytes * h
              sourceSize:MTLSizeMake(w, h, 1) toTexture:r->texture destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [e endEncoding];
        transientBytes += rowBytes * h;
        // Asset loading can upload many textures before the first BeginFrame.
        // Bound staging lifetime independently of presentation/frame cadence.
        if (transientBytes >= kSubmissionBudget) Submit();
    }
    bool Read(id<MTLTexture> texture, std::vector<uint8_t>& pixels, int& pitch, const TVPLayerRect* region = nullptr)
    {
        pitch = 0;
        const int x=region ? region->left : 0, y=region ? region->top : 0;
        const int regionWidth=region ? region->Width() : int(texture.width);
        const int regionHeight=region ? region->Height() : int(texture.height);
        if(x<0 || y<0 || regionWidth<=0 || regionHeight<=0 ||
           size_t(x)+regionWidth>texture.width || size_t(y)+regionHeight>texture.height) return false;
        const size_t w=regionWidth,h=regionHeight;
        const size_t bpp = texture.pixelFormat == MTLPixelFormatR8Unorm ? 1 : 4;
        const size_t rowBytes = (w * bpp + 255) & ~size_t(255);
        id<MTLBuffer> staging = AcquireStaging(rowBytes * h);
        if (!staging) return false;
        id<MTLBlitCommandEncoder> e = Blit();
        if (!e) return false;
        [e copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
                sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0
                destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * h];
        [e endEncoding];
        if (!Submit(true)) return false;
        pixels.resize(w * h * bpp);
        pitch = static_cast<int>(w * bpp);
        for (size_t y = 0; y < h; ++y)
            std::memcpy(pixels.data() + y * pitch, static_cast<uint8_t*>(staging.contents) + y * rowBytes, pitch);
        return true;
    }
    id<MTLTexture> Snapshot(Resource* r)
    {
        if (!destinationSnapshot || destinationSnapshot.width != static_cast<NSUInteger>(r->width) ||
            destinationSnapshot.height != static_cast<NSUInteger>(r->height))
            destinationSnapshot = Texture(r->width, r->height);
        if (!destinationSnapshot) throw std::runtime_error("Metal snapshot texture allocation failed");
        id<MTLBlitCommandEncoder> e = Blit();
        if (!e) throw std::runtime_error("Metal blit encoder allocation failed");
        [e copyFromTexture:r->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(r->width, r->height, 1) toTexture:destinationSnapshot
                destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [e endEncoding];
        return destinationSnapshot;
    }
    void DrawWindows(id<MTLTexture> output, id<MTLRenderPipelineState> pipeline)
    {
        id<MTLRenderCommandEncoder> e = Pass(output, true);
        [e setRenderPipelineState:pipeline];
        for (const auto& draw : windows) {
            float l = draw.x / width * 2 - 1, t = draw.y / height * 2 - 1;
            float r = (draw.x + draw.width) / width * 2 - 1;
            float b = (draw.y + draw.height) / height * 2 - 1;
            const float vertices[] = {l,t,0,0, r,t,1,0, r,b,1,1, l,t,0,0, r,b,1,1, l,b,0,1};
            [e setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
            [e setFragmentTexture:draw.texture atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        }
        [e endEncoding];
    }
    id<MTLRenderPipelineState> Pipeline(id<MTLLibrary> lib, NSString* vertex, NSString* fragment,
                                       MTLPixelFormat format, int blend)
    {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:vertex];
        d.fragmentFunction = [lib newFunctionWithName:fragment];
        auto a = d.colorAttachments[0];
        a.pixelFormat = format;
        if (blend >= 0) {
            a.blendingEnabled = YES;
            a.sourceRGBBlendFactor = blend == 1 ? MTLBlendFactorDestinationColor : MTLBlendFactorSourceAlpha;
            a.destinationRGBBlendFactor = blend == 1 ? MTLBlendFactorOne : MTLBlendFactorOneMinusSourceAlpha;
            a.sourceAlphaBlendFactor = blend == 1 ? MTLBlendFactorZero :
                                      blend == 2 ? MTLBlendFactorSourceAlpha : MTLBlendFactorOne;
            a.destinationAlphaBlendFactor = blend == 2 ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
            a.alphaBlendOperation = blend == 0 ? MTLBlendOperationMax : MTLBlendOperationAdd;
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&error];
        if (!p) SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Metal pipeline: %s", error.localizedDescription.UTF8String);
        return p;
    }
    DeformTopology* DeformGrid(int divX, int divY)
    {
        if (divX < 1 || divY < 1 || divX > 254 || divY > 254) return nullptr;
        const uint32_t key = (static_cast<uint32_t>(divX) << 16) | static_cast<uint32_t>(divY);
        auto found = deformTopologies.find(key);
        if (found != deformTopologies.end()) return &found->second;

        const int vertexCount = (divX + 1) * (divY + 1);
        const int indexCount = divX * divY * 6;
        std::vector<simd_float2> uv(static_cast<size_t>(vertexCount));
        size_t vertex = 0;
        for (int gy = 0; gy <= divY; ++gy)
            for (int gx = 0; gx <= divX; ++gx)
                uv[vertex++] = simd_make_float2(static_cast<float>(gx) / divX,
                                                static_cast<float>(gy) / divY);
        std::vector<uint16_t> indices(static_cast<size_t>(indexCount));
        size_t index = 0;
        for (int gy = 0; gy < divY; ++gy) {
            for (int gx = 0; gx < divX; ++gx) {
                const uint16_t i0 = static_cast<uint16_t>(gy * (divX + 1) + gx);
                const uint16_t i1 = static_cast<uint16_t>(i0 + 1);
                const uint16_t i2 = static_cast<uint16_t>((gy + 1) * (divX + 1) + gx);
                const uint16_t i3 = static_cast<uint16_t>(i2 + 1);
                indices[index++] = i0; indices[index++] = i1; indices[index++] = i2;
                indices[index++] = i1; indices[index++] = i3; indices[index++] = i2;
            }
        }
        DeformTopology topology;
        topology.uv = [device newBufferWithBytes:uv.data()
                                          length:uv.size() * sizeof(simd_float2)
                                         options:MTLResourceStorageModeShared];
        topology.indices = [device newBufferWithBytes:indices.data()
                                               length:indices.size() * sizeof(uint16_t)
                                              options:MTLResourceStorageModeShared];
        if (!topology.uv || !topology.indices)
            throw std::runtime_error("Metal deformation topology allocation failed");
        topology.vertexCount = vertexCount;
        topology.indexCount = indexCount;
        auto inserted = deformTopologies.emplace(key, topology);
        return &inserted.first->second;
    }

    bool Initialize(SDL_Window* w, bool vsync)
    {
        window = w;
        device = MTLCreateSystemDefaultDevice();
        if (!device) return false;
        view = SDL_Metal_CreateView(window);
        if (!view) return false;
        layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
        if (!layer) return false;
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
        // One surface can be on screen while two frame submissions are being
        // prepared/presented. Drawable availability is separate from GPU work.
        layer.maximumDrawableCount = 3;
#if TARGET_OS_OSX
        layer.displaySyncEnabled = vsync;
#else
        (void)vsync; // iOS presentation is paced by the host CADisplayLink.
#endif
        queue = [device newCommandQueue];
        if (!queue) return false;
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaders]
                                                options:nil error:&error];
        if (!lib) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Metal shaders: %s", error.localizedDescription.UTF8String);
            return false;
        }
        windowPipeline = Pipeline(lib, @"vertexMain", @"windowMain", MTLPixelFormatBGRA8Unorm, -1);
        capturePipeline = Pipeline(lib, @"vertexMain", @"windowMain", MTLPixelFormatRGBA8Unorm, -1);
        for (int i = 0; i < 3; ++i) {
            meshPipelines[i] = Pipeline(lib, @"vertexMain", @"meshMain", MTLPixelFormatRGBA8Unorm, i);
            deformPipelines[i] = Pipeline(lib, @"deformVertexMain", @"meshMain", MTLPixelFormatRGBA8Unorm, i);
        }
        layerPipeline = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"layerMain"] error:&error];
        if (!layerPipeline) SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Metal layer pipeline: %s", error.localizedDescription.UTF8String);
        MTLCompileOptions* ordinaryOptions = [MTLCompileOptions new];
        ordinaryOptions.fastMathEnabled = NO;
        id<MTLLibrary> ordinary = [device newLibraryWithSource:[NSString stringWithUTF8String:kMetalLayerShaders]
                                                      options:ordinaryOptions error:&error];
        if (ordinary) {
            ordinaryLayerPipeline = [device newComputePipelineStateWithFunction:
                                     [ordinary newFunctionWithName:@"ordinaryLayer"] error:&error];
            dualSourceLayerPipeline = [device newComputePipelineStateWithFunction:
                                       [ordinary newFunctionWithName:@"dualSourceLayer"] error:&error];
        }
        if(ordinaryLayerPipeline && device.readWriteTextureSupport == MTLReadWriteTextureTier2) {
            // These kernels read only their own destination pixel, into registers,
            // before overwriting it. Aliased sources still require region snapshots.
            std::string inPlaceSource("#define TVP_LAYER_IN_PLACE 1\n");
            inPlaceSource+=kMetalLayerShaders;
            id<MTLLibrary> inPlace=[device newLibraryWithSource:[NSString stringWithUTF8String:inPlaceSource.c_str()]
                                                      options:ordinaryOptions error:&error];
            if(inPlace) ordinaryInPlacePipeline=[device newComputePipelineStateWithFunction:
                                                  [inPlace newFunctionWithName:@"ordinaryLayer"] error:&error];
            if(!ordinaryInPlacePipeline) SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                "GPU Layer in-place optimization unavailable; region snapshots retained: %s",error.localizedDescription.UTF8String);
        }
        if (!ordinaryLayerPipeline) SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
            "GPU Layer initialization failed; software composition retained: %s", error.localizedDescription.UTF8String);
        if (!dualSourceLayerPipeline) SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
            "GPU Layer dual-source pipeline unavailable; SD transitions use software fallback: %s",
            error.localizedDescription.UTF8String);
        return windowPipeline && capturePipeline && meshPipelines[0] && meshPipelines[1] &&
               meshPipelines[2] && deformPipelines[0] && deformPipelines[1] &&
               deformPipelines[2] && layerPipeline;
    }
};

MetalRenderBackend::MetalRenderBackend() : impl_(std::make_unique<Impl>()) {}
MetalRenderBackend::~MetalRenderBackend() = default;
iTVPRenderBackend* MetalRenderBackend::Create(SDL_Window* window, bool vsync)
{
    @autoreleasepool {
        auto backend = std::unique_ptr<MetalRenderBackend>(new MetalRenderBackend());
        if (!window || !backend->impl_->Initialize(window, vsync)) return nullptr;
        return backend.release();
    }
}
bool MetalRenderBackendAvailable()
{
    @autoreleasepool { return MTLCreateSystemDefaultDevice() != nil; }
}
void MetalRenderBackend::FetchInfo()
{
    SDL_Log("Native Metal backend: %s (GPU textures, on-demand CPU readback)", impl_->device.name.UTF8String);
}
double MetalRenderBackend::GetGpuSubmissionTimeMilliseconds() const
{
    return impl_->gpuTimeMS->load(std::memory_order_relaxed);
}
double MetalRenderBackend::GetPresentationWaitTimeMilliseconds() const
{
    return impl_->lastPresentationWaitMS;
}
void MetalRenderBackend::BeginFrame(int w, int h)
{
    impl_->width = w; impl_->height = h;
    impl_->windows.clear();
}
void MetalRenderBackend::EndFrame()
{
    @autoreleasepool {
        auto& p = *impl_;
        double drawableWaitMS = 0;
        if (p.width > 0 && p.height > 0 && !(SDL_GetWindowFlags(p.window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))) {
            if (p.layer.drawableSize.width != p.width || p.layer.drawableSize.height != p.height)
                p.layer.drawableSize = CGSizeMake(p.width, p.height);
            const Uint64 waitStarted = SDL_GetTicksNS();
            id<CAMetalDrawable> drawable = [p.layer nextDrawable];
            drawableWaitMS = static_cast<double>(SDL_GetTicksNS() - waitStarted) / 1000000.0;
            if (drawable) {
                p.DrawWindows(drawable.texture, p.windowPipeline);
                [p.Commands() presentDrawable:drawable];
            }
        }
        p.Submit(); // Offscreen work must still complete when no drawable is available.
        p.lastPresentationWaitMS = p.pendingQueueWaitMS + drawableWaitMS;
        p.pendingQueueWaitMS = 0;
    }
}
void* MetalRenderBackend::CreateTexture(int w, int h) { @autoreleasepool { return impl_->Create(w, h, false); } }
void* MetalRenderBackend::CreateTarget(int w, int h) { @autoreleasepool { return impl_->Create(w, h, true); } }
void* MetalRenderBackend::CreateWindowTexture(int w, int h) { return CreateTexture(w, h); }
void MetalRenderBackend::UpdateTexture(void* h, const uint8_t* pixels, int w, int height, int pitch)
{
    @autoreleasepool { impl_->Upload(impl_->Find(h), pixels, w, height, pitch); }
}
void MetalRenderBackend::UpdateWindowTexture(void* h, const uint8_t* p, int w, int height, int pitch) { UpdateTexture(h, p, w, height, pitch); }
void MetalRenderBackend::UpdateTargetTexture(void* h, const uint8_t* p, int w, int height, int pitch) { UpdateTexture(h, p, w, height, pitch); }
void MetalRenderBackend::DestroyTexture(void* h) { impl_->Destroy(h, false); }
void MetalRenderBackend::DestroyWindowTexture(void* h) { DestroyTexture(h); }
void MetalRenderBackend::DestroyTarget(void* h) { impl_->Destroy(h, true); }
void MetalRenderBackend::SetTarget(void* h)
{
    auto r = impl_->Find(h);
    impl_->current = r && r->target ? r : nullptr;
}
void MetalRenderBackend::SetMask(void* h)
{
    auto r = impl_->Find(h);
    impl_->mask = r && r->target ? r : nullptr;
}
void* MetalRenderBackend::GetTargetTexture(void* h)
{
    auto r = impl_->Find(h);
    return r && r->target ? h : nullptr;
}
void MetalRenderBackend::ClearTarget(bool clear)
{
    @autoreleasepool { if (clear && impl_->current) [impl_->Pass(impl_->current->texture, true) endEncoding]; }
}
uint8_t* MetalRenderBackend::LockTarget(void* h, int& pitch)
{
    @autoreleasepool {
        pitch = 0;
        auto r = impl_->Find(h);
        if (!r || !r->target || !impl_->Read(r->texture, r->readback, pitch)) return nullptr;
        return r->readback.data();
    }
}
void MetalRenderBackend::UnlockTarget(void* h)
{
    if (auto r = impl_->Find(h)) std::vector<uint8_t>().swap(r->readback);
}
void MetalRenderBackend::DrawWindowTexture(void* h, float x, float y, float w, float height)
{
    if (w <= 0 || height <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(w) || !std::isfinite(height)) return;
    if (auto r = impl_->Find(h)) impl_->windows.push_back({r->texture, x, y, w, height});
}
void MetalRenderBackend::SetBlendMode(int mode, const float* color)
{
    impl_->mesh.flags.x = mode;
    impl_->mesh.flags.z = mode == 21 && color != nullptr;
    if (color) impl_->mesh.color = Color(color);
}
void MetalRenderBackend::DrawMesh(const float* vertices, int vertexCount, const uint16_t* indices,
                                 int indexCount, void* handle, float opacity, const float* modulation)
{
    @autoreleasepool {
        auto& p = *impl_;
        auto r = p.Find(handle);
        if (!p.current || !r || !vertices || !indices || vertexCount <= 0 || indexCount < 3 || p.mesh.flags.x == 6) return;
        const Uint64 cpuStarted = SDL_GetTicksNS();
        const Uint64 validationStarted = cpuStarted;
        for (int n = 0; n < indexCount; ++n) if (indices[n] >= vertexCount) return;
        const Uint64 validationTimeNS = SDL_GetTicksNS() - validationStarted;
        id<MTLTexture> source = r->texture;
        // Avoid sampling the render attachment, including when it is also the mask.
        id<MTLTexture> snapshot = (r == p.current || p.mask == p.current) ? p.Snapshot(p.current) : nil;
        if (r == p.current) source = snapshot;
        const size_t vertexBytes = static_cast<size_t>(vertexCount) * 4 * sizeof(float);
        const size_t indexBytes = static_cast<size_t>(indexCount) * sizeof(uint16_t);
        const size_t indexOffset = Impl::AlignRing(vertexBytes);
        const size_t uploadBytes = indexOffset + indexBytes;
        auto upload = p.AcquireRing(uploadBytes);
        if (!upload.buffer) throw std::runtime_error("Metal mesh ring allocation failed");
        auto* uploadBytesPtr = static_cast<uint8_t*>(upload.buffer.contents) + upload.offset;
        std::memcpy(uploadBytesPtr, vertices, vertexBytes);
        std::memcpy(uploadBytesPtr + indexOffset, indices, indexBytes);
        Params uniforms = p.mesh;
        uniforms.modulation = modulation ? Color(modulation) : simd_make_float4(1, 1, 1, 1);
        uniforms.flags.y = p.mask != nullptr;
        uniforms.values = simd_make_float4(std::clamp(opacity, 0.0f, 1.0f), p.current->width, p.current->height, 0);
        int pipeline = (uniforms.flags.x == 1 || uniforms.flags.x == 4) ? 1 : uniforms.flags.x == 21 ? 2 : 0;
        id<MTLRenderCommandEncoder> e = p.Pass(p.current->texture, false);
        [e setRenderPipelineState:p.meshPipelines[pipeline]];
        [e setVertexBuffer:upload.buffer offset:upload.offset atIndex:0];
        [e setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [e setFragmentTexture:source atIndex:0];
        [e setFragmentTexture:p.mask ? (p.mask == p.current ? snapshot : p.mask->texture) : source atIndex:1];
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:indexCount indexType:MTLIndexTypeUInt16
                     indexBuffer:upload.buffer indexBufferOffset:upload.offset + indexOffset];
        [e endEncoding];
        p.transientBytes += uploadBytes;
        if (p.transientBytes >= Impl::kSubmissionBudget) p.Submit();
        TVPRecordMeshDraw(static_cast<uint64_t>(vertexCount),static_cast<uint64_t>(indexCount),
                          SDL_GetTicksNS() - cpuStarted,validationTimeNS,0,
                          static_cast<uint64_t>(vertexBytes + indexBytes),0);
    }
}
bool MetalRenderBackend::DrawDeformedMesh(int divX, int divY,
                                          const TVPMeshDeformSurface* surfaces,
                                          int surfaceCount, void* handle, float opacity,
                                          const float* modulation)
{
    @autoreleasepool {
        auto& p = *impl_;
        auto r = p.Find(handle);
        if (!p.current || !r || !surfaces || surfaceCount <= 0 || p.mesh.flags.x == 6)
            return false;
        auto* topology = p.DeformGrid(divX, divY);
        if (!topology) return false;

        const Uint64 cpuStarted = SDL_GetTicksNS();
        const size_t surfaceBytes = static_cast<size_t>(surfaceCount) * sizeof(TVPMeshDeformSurface);
        auto surfaceUpload = p.AcquireRing(surfaceBytes);
        if (!surfaceUpload.buffer)
            throw std::runtime_error("Metal deformation parameter ring allocation failed");
        std::memcpy(static_cast<uint8_t*>(surfaceUpload.buffer.contents) + surfaceUpload.offset,
                    surfaces, surfaceBytes);

        id<MTLTexture> source = r->texture;
        id<MTLTexture> snapshot = (r == p.current || p.mask == p.current) ? p.Snapshot(p.current) : nil;
        if (r == p.current) source = snapshot;

        Params uniforms = p.mesh;
        uniforms.modulation = modulation ? Color(modulation) : simd_make_float4(1, 1, 1, 1);
        uniforms.flags.y = p.mask != nullptr;
        uniforms.values = simd_make_float4(std::clamp(opacity, 0.0f, 1.0f),
                                           p.current->width, p.current->height, 0);
        const int pipeline = (uniforms.flags.x == 1 || uniforms.flags.x == 4) ? 1 :
                             uniforms.flags.x == 21 ? 2 : 0;
        const uint32_t count = static_cast<uint32_t>(surfaceCount);
        id<MTLRenderCommandEncoder> e = p.Pass(p.current->texture, false);
        [e setRenderPipelineState:p.deformPipelines[pipeline]];
        [e setVertexBuffer:topology->uv offset:0 atIndex:0];
        [e setVertexBuffer:surfaceUpload.buffer offset:surfaceUpload.offset atIndex:1];
        [e setVertexBytes:&count length:sizeof(count) atIndex:2];
        [e setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [e setFragmentTexture:source atIndex:0];
        [e setFragmentTexture:p.mask ? (p.mask == p.current ? snapshot : p.mask->texture) : source
                         atIndex:1];
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                       indexCount:topology->indexCount
                        indexType:MTLIndexTypeUInt16
                      indexBuffer:topology->indices
                indexBufferOffset:0];
        [e endEncoding];

        p.transientBytes += surfaceBytes;
        if (p.transientBytes >= Impl::kSubmissionBudget) p.Submit();
        TVPRecordEmoteGPUDeform(static_cast<uint64_t>(topology->vertexCount));
        TVPRecordMeshDraw(static_cast<uint64_t>(topology->vertexCount),
                          static_cast<uint64_t>(topology->indexCount),
                          SDL_GetTicksNS() - cpuStarted, 0, 0,
                          static_cast<uint64_t>(surfaceBytes), 0);
        return true;
    }
}
void MetalRenderBackend::LayerSetBlend(int method, float opacity, const float* color)
{
    impl_->layerParams.flags.z = method;
    impl_->layerParams.flags.w = static_cast<int>(std::clamp(opacity, 0.0f, 1.0f) * 255 + 0.5f);
    if (color) impl_->layerParams.color = Color(color);
}
void MetalRenderBackend::LayerDrawRect(void* h, float x, float y, float w, float height,
                                     float u0, float v0, float u1, float v1)
{
    @autoreleasepool {
        auto& p = *impl_;
        auto r = p.Find(h);
        if (!p.current || !r || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
            !std::isfinite(height) || !std::isfinite(u0) || !std::isfinite(v0) ||
            !std::isfinite(u1) || !std::isfinite(v1) || w <= 0 || height <= 0 ||
            !std::isfinite(x + w) || !std::isfinite(y + height)) return;
        // MSL coverage uses integer pixel coordinates. Reject values outside
        // their representable range before converting floor/ceil to int2.
        constexpr float limit = 1073741824.0f;
        if (std::abs(x) > limit || std::abs(y) > limit || std::abs(x + w) > limit ||
            std::abs(y + height) > limit) return;
        float l = std::max(0.0f, std::floor(x)), t = std::max(0.0f, std::floor(y));
        float right = std::min(static_cast<float>(p.current->width), std::ceil(x + w));
        float bottom = std::min(static_cast<float>(p.current->height), std::ceil(y + height));
        if (l >= right || t >= bottom) return;
        id<MTLTexture> dst = p.Snapshot(p.current);
        Params uniforms = p.layerParams;
        uniforms.rect = simd_make_float4(x, y, w, height);
        uniforms.uv = simd_make_float4(u0, v0, u1, v1);
        id<MTLComputeCommandEncoder> e = p.Compute();
        if (!e) throw std::runtime_error("Metal compute encoder allocation failed");
        [e setComputePipelineState:p.layerPipeline];
        [e setBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [e setTexture:r == p.current ? dst : r->texture atIndex:0];
        [e setTexture:dst atIndex:1];
        [e setTexture:p.current->texture atIndex:2];
        [e dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(right - l), static_cast<NSUInteger>(bottom - t), 1)
             threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [e endEncoding];
    }
}
bool MetalRenderBackend::CaptureFrame(std::vector<uint8_t>& pixels, int& w, int& h, int& pitch)
{
    @autoreleasepool {
        auto& p = *impl_;
        w = h = pitch = 0;
        if (p.width <= 0 || p.height <= 0 || p.windows.empty()) return false;
        id<MTLTexture> output = p.Texture(p.width, p.height);
        if (!output) return false;
        p.DrawWindows(output, p.capturePipeline);
        if (!p.Read(output, pixels, pitch)) return false;
        w = p.width; h = p.height;
        return true;
    }
}
namespace
{
struct MetalRegistration
{
    MetalRegistration()
    {
        TVPRegisterRenderBackend({"metal", "Native Metal renderer", MetalRenderBackendAvailable, nullptr});
    }
} metalRegistration;
}
bool MetalRenderBackend::SupportsLayerOperations() const { return impl_->ordinaryLayerPipeline != nil; }
bool MetalRenderBackend::SetLayerAlphaTables(const uint8_t* opacity, const uint8_t* negative) {
    @autoreleasepool {
        if (!opacity || !negative || !SupportsLayerOperations()) return false;
        impl_->alphaTables = [impl_->device newBufferWithLength:131072 options:MTLResourceStorageModeShared];
        if (!impl_->alphaTables) return false;
        std::memcpy(impl_->alphaTables.contents, opacity, 65536);
        std::memcpy(static_cast<uint8_t*>(impl_->alphaTables.contents)+65536, negative, 65536);
        return true;
    }
}
void* MetalRenderBackend::CreateLayerTexture(int w, int h, TVPLayerTextureFormat format) {
    @autoreleasepool {
        auto& p = *impl_;
        if (!SupportsLayerOperations()) return nullptr;
        auto r = std::make_unique<Impl::Resource>();
        r->bytesPerPixel = format == TVPLayerTextureFormat::R8 ? 1 : 4;
        r->texture = p.Texture(w,h,r->bytesPerPixel == 1 ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm);
        if (!r->texture) return nullptr;
        r->width=w; r->height=h; r->target=true;
        [p.Pass(r->texture,true) endEncoding];
        void* handle=r.get(); p.resources.emplace(handle,std::move(r)); return handle;
    }
}
void MetalRenderBackend::DestroyLayerTexture(void* handle) { impl_->Destroy(handle,true); }
bool MetalRenderBackend::UpdateLayerTexture(void* handle, const uint8_t* pixels, int pitch, const TVPLayerRect& rc) {
    @autoreleasepool {
        auto& p=*impl_; auto* r=p.Find(handle);
        if (!r || !pixels || rc.left<0 || rc.top<0 || rc.right>r->width || rc.bottom>r->height ||
            rc.Width()<=0 || rc.Height()<=0 || pitch<rc.Width()*r->bytesPerPixel) return false;
        size_t bytes=rc.Width()*r->bytesPerPixel, row=(bytes+255)&~size_t(255);
        id<MTLBuffer> staging=p.AcquireStaging(row*rc.Height());
        if (!staging) return false;
        for(int y=0;y<rc.Height();++y) std::memcpy(static_cast<uint8_t*>(staging.contents)+y*row,pixels+y*pitch,bytes);
        id<MTLBlitCommandEncoder> e=p.Blit(); if(!e) return false;
        [e copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row*rc.Height()
              sourceSize:MTLSizeMake(rc.Width(),rc.Height(),1) toTexture:r->texture destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(rc.left,rc.top,0)];
        [e endEncoding]; p.transientBytes+=row*rc.Height();
        if(p.transientBytes>=Impl::kSubmissionBudget) p.Submit(); return true;
    }
}
bool MetalRenderBackend::CopyTargetToLayerTexture(void* sourceHandle, void* destinationHandle) {
    @autoreleasepool {
        auto& p=*impl_;
        auto* source=p.Find(sourceHandle);
        auto* destination=p.Find(destinationHandle);
        if(!source || !destination || source==destination || !source->target || !destination->target ||
           source->bytesPerPixel!=4 || destination->bytesPerPixel!=4 ||
           source->width!=destination->width || source->height!=destination->height ||
           source->width<=0 || source->height<=0) return false;
        id<MTLBlitCommandEncoder> e=p.Blit();
        if(!e) return false;
        [e copyFromTexture:source->texture sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0,0,0)
              sourceSize:MTLSizeMake(source->width,source->height,1)
              toTexture:destination->texture destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0,0,0)];
        [e endEncoding];
        p.transientBytes+=size_t(source->width)*source->height*4;
        if(p.transientBytes>=Impl::kSubmissionBudget) p.Submit();
        return true;
    }
}
bool MetalRenderBackend::ReadLayerTexture(void* handle,std::vector<uint8_t>& pixels,int& pitch) {
    @autoreleasepool { auto* r=impl_->Find(handle); return r && impl_->Read(r->texture,pixels,pitch); }
}
bool MetalRenderBackend::ReadLayerTextureRegion(void* handle,const TVPLayerRect& region,std::vector<uint8_t>& pixels,int& pitch) {
    @autoreleasepool { auto* r=impl_->Find(handle); return r && impl_->Read(r->texture,pixels,pitch,&region); }
}
bool MetalRenderBackend::OperateLayerRect(const TVPLayerOperation& operation,void* target,const TVPLayerRect& dst,
                                         void* source,const TVPLayerRect& src,int sampling) {
    @autoreleasepool {
        auto& p=*impl_; auto* t=p.Find(target); auto* s=p.Find(source);
        int kind=static_cast<int>(operation.kind);
        bool needsSource=kind<5 || (kind>=8 && kind<=10);
        if(!p.ordinaryLayerPipeline || !t || t->bytesPerPixel!=4 || kind==0 || (needsSource && !s) ||
            dst.Width()<=0 || dst.Height()<=0 || sampling<0 || sampling>1) return false;
        if(needsSource && (src.Width()==0 || src.Height()==0 || std::min(src.left,src.right)<0 ||
            std::min(src.top,src.bottom)<0 || std::max(src.left,src.right)>s->width || std::max(src.top,src.bottom)>s->height)) return false;
        if((operation.flags & TVP_LAYER_DEST_ALPHA) && !p.alphaTables) return false;
        TVPLayerRect clip={std::max(0,dst.left),std::max(0,dst.top),std::min(t->width,dst.right),std::min(t->height,dst.bottom)};
        if(clip.Width()<=0 || clip.Height()<=0) return true;
        bool overwrite=kind==1 || kind==4 || kind==5;
        bool inPlace=p.ordinaryInPlacePipeline!=nil;
        id<MTLTexture> snapshot=nil, sourceTexture=s ? s->texture : p.ordinaryDummy;
        if(!sourceTexture) { p.ordinaryDummy=p.Texture(1,1); sourceTexture=p.ordinaryDummy; }
        // Snapshot only affected destination pixels. Source aliases need a
        // separate region snapshot before any write, including overlapping copies.
        if((!inPlace && !overwrite) || s==t) {
            id<MTLBlitCommandEncoder> blit=p.Blit(); if(!blit) return false;
            if(!inPlace && !overwrite) {
                if(!p.ordinarySnapshot || p.ordinarySnapshot.width!=NSUInteger(clip.Width()) || p.ordinarySnapshot.height!=NSUInteger(clip.Height()))
                    p.ordinarySnapshot=p.Texture(clip.Width(),clip.Height());
                snapshot=p.ordinarySnapshot; if(!snapshot) { [blit endEncoding]; return false; }
                [blit copyFromTexture:t->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(clip.left,clip.top,0)
                      sourceSize:MTLSizeMake(clip.Width(),clip.Height(),1) toTexture:snapshot destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            }
            if(s==t) {
                int x=std::min(src.left,src.right),y=std::min(src.top,src.bottom);
                if(!p.ordinarySourceSnapshot || p.ordinarySourceSnapshot.width!=NSUInteger(std::abs(src.Width())) || p.ordinarySourceSnapshot.height!=NSUInteger(std::abs(src.Height())))
                    p.ordinarySourceSnapshot=p.Texture(std::abs(src.Width()),std::abs(src.Height()));
                sourceTexture=p.ordinarySourceSnapshot;
                if(!sourceTexture) { [blit endEncoding]; return false; }
                [blit copyFromTexture:s->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x,y,0)
                      sourceSize:MTLSizeMake(std::abs(src.Width()),std::abs(src.Height()),1) toTexture:sourceTexture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            }
            [blit endEncoding];
        }
        struct LayerParameters { simd_int4 destination,source,clip,operation,color; } params;
        params.destination={dst.left,dst.top,dst.right,dst.bottom};
        params.source={src.left,src.top,src.right,src.bottom};
        if(s==t) { int x=std::min(src.left,src.right),y=std::min(src.top,src.bottom); params.source-=simd_int4{x,y,x,y}; }
        params.clip={clip.left,clip.top,clip.right,clip.bottom};
        params.operation={kind,operation.opacity,static_cast<int>(operation.flags),sampling};
        params.color={int(operation.color&255),int((operation.color>>8)&255),int((operation.color>>16)&255),int(operation.color>>24)};
        if(!p.alphaTables) p.alphaTables=[p.device newBufferWithLength:131072 options:MTLResourceStorageModeShared];
        id<MTLComputeCommandEncoder> e=inPlace ? p.OrdinaryCompute() : p.Compute(); if(!e) return false;
        [e setBytes:&params length:sizeof(params) atIndex:0];
        if(inPlace) {
            if(p.ordinaryBoundSource!=sourceTexture) { [e setTexture:sourceTexture atIndex:0]; p.ordinaryBoundSource=sourceTexture; }
            if(p.ordinaryBoundTarget!=t->texture) { [e setTexture:t->texture atIndex:2]; p.ordinaryBoundTarget=t->texture; }
        } else {
            [e setComputePipelineState:p.ordinaryLayerPipeline];
            [e setBuffer:p.alphaTables offset:0 atIndex:1];
            [e setTexture:sourceTexture atIndex:0]; [e setTexture:snapshot ? snapshot : sourceTexture atIndex:1]; [e setTexture:t->texture atIndex:2];
        }
        [e dispatchThreads:MTLSizeMake(clip.Width(),clip.Height(),1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
        if(!inPlace) [e endEncoding];
        p.transientBytes+=size_t(clip.Width())*clip.Height()*4;
        if(p.transientBytes>=Impl::kSubmissionBudget) p.Submit();
        return true;
    }
}
bool MetalRenderBackend::OperateLayerRectDualSource(const TVPLayerOperation& operation,
                                                    void* target,const TVPLayerRect& dst,
                                                    void* source1,const TVPLayerRect& src1,
                                                    void* source2,const TVPLayerRect& src2) {
    @autoreleasepool {
        auto& p=*impl_;
        auto* t=p.Find(target); auto* s1=p.Find(source1); auto* s2=p.Find(source2);
        if(!p.dualSourceLayerPipeline || !t || !s1 || !s2 ||
           operation.kind!=TVPLayerOperationKind::ConstAlphaSD ||
           t->bytesPerPixel!=4 || s1->bytesPerPixel!=4 || s2->bytesPerPixel!=4 ||
           operation.opacity<0 || operation.opacity>255) return false;
        const int w=dst.Width(),h=dst.Height();
        if(w<=0 || h<=0 || src1.Width()!=w || src1.Height()!=h ||
           src2.Width()!=w || src2.Height()!=h) return false;
        const auto sameRect=[](const TVPLayerRect& a,const TVPLayerRect& b) {
            return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
        };
        if((s1==t && !sameRect(src1,dst)) || (s2==t && !sameRect(src2,dst))) return false;
        auto validRect=[](const TVPLayerRect& r,const Impl::Resource* s) {
            return r.left>=0 && r.top>=0 && r.right<=s->width && r.bottom<=s->height &&
                   r.Width()>0 && r.Height()>0;
        };
        if(!validRect(src1,s1) || !validRect(src2,s2)) return false;
        if((operation.flags & TVP_LAYER_DEST_ALPHA) && !p.alphaTables) return false;
        TVPLayerRect clip={std::max(0,dst.left),std::max(0,dst.top),
                           std::min(t->width,dst.right),std::min(t->height,dst.bottom)};
        if(clip.Width()<=0 || clip.Height()<=0) return true;

        id<MTLTexture> tex1=s1->texture,tex2=s2->texture;
        TVPLayerRect actual1=src1,actual2=src2;
        if(s1==t || s2==t) {
            id<MTLBlitCommandEncoder> blit=p.Blit(); if(!blit) return false;
            if(s1==t) {
                if(!p.dualSourceSnapshot1 ||
                   p.dualSourceSnapshot1.width!=NSUInteger(w) ||
                   p.dualSourceSnapshot1.height!=NSUInteger(h))
                    p.dualSourceSnapshot1=p.Texture(w,h);
                tex1=p.dualSourceSnapshot1;
                if(!tex1) { [blit endEncoding]; return false; }
                [blit copyFromTexture:s1->texture sourceSlice:0 sourceLevel:0
                      sourceOrigin:MTLOriginMake(src1.left,src1.top,0)
                      sourceSize:MTLSizeMake(w,h,1) toTexture:tex1 destinationSlice:0
                      destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
                actual1={0,0,w,h};
            }
            if(s2==t) {
                if(!p.dualSourceSnapshot2 ||
                   p.dualSourceSnapshot2.width!=NSUInteger(w) ||
                   p.dualSourceSnapshot2.height!=NSUInteger(h))
                    p.dualSourceSnapshot2=p.Texture(w,h);
                tex2=p.dualSourceSnapshot2;
                if(!tex2) { [blit endEncoding]; return false; }
                [blit copyFromTexture:s2->texture sourceSlice:0 sourceLevel:0
                      sourceOrigin:MTLOriginMake(src2.left,src2.top,0)
                      sourceSize:MTLSizeMake(w,h,1) toTexture:tex2 destinationSlice:0
                      destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
                actual2={0,0,w,h};
            }
            [blit endEncoding];
        }
        struct DualLayerParameters { simd_int4 destination,source1,source2,clip,operation; } params;
        params.destination={dst.left,dst.top,dst.right,dst.bottom};
        params.source1={actual1.left,actual1.top,actual1.right,actual1.bottom};
        params.source2={actual2.left,actual2.top,actual2.right,actual2.bottom};
        params.clip={clip.left,clip.top,clip.right,clip.bottom};
        params.operation={static_cast<int>(operation.kind),operation.opacity,
                          static_cast<int>(operation.flags),0};
        id<MTLComputeCommandEncoder> e=p.Compute(); if(!e) return false;
        [e setComputePipelineState:p.dualSourceLayerPipeline];
        [e setBytes:&params length:sizeof(params) atIndex:0];
        [e setBuffer:p.alphaTables offset:0 atIndex:1];
        [e setTexture:tex1 atIndex:0];
        [e setTexture:tex2 atIndex:1];
        [e setTexture:t->texture atIndex:2];
        [e dispatchThreads:MTLSizeMake(clip.Width(),clip.Height(),1)
             threadsPerThreadgroup:MTLSizeMake(8,8,1)];
        [e endEncoding];
        p.transientBytes+=size_t(clip.Width())*clip.Height()*4;
        if(p.transientBytes>=Impl::kSubmissionBudget) p.Submit();
        return true;
    }
}

} // namespace krkrsdl3
