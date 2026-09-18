#include "MetalRenderBackend.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <simd/simd.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
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
struct Params { float4 color, modulation, rect, uv; int4 flags; float4 values; };
vertex Varying vertexMain(uint i [[vertex_id]], const device Vertex* v [[buffer(0)]]) {
    return {float4(v[i].xy.x, -v[i].xy.y, 0, 1), v[i].uv};
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
    id<MTLComputePipelineState> layerPipeline = nil;
    id<MTLTexture> destinationSnapshot = nil;
    dispatch_semaphore_t inFlight = dispatch_semaphore_create(2);
    std::shared_ptr<std::atomic<bool>> gpuFailed = std::make_shared<std::atomic<bool>>(false);
    std::unordered_map<void*, std::unique_ptr<Resource>> resources;
    std::vector<WindowDraw> windows;
    Resource* current = nullptr;
    Resource* mask = nullptr;
    Params mesh, layerParams;
    int width = 0, height = 0;
    size_t transientBytes = 0;
    static constexpr size_t kSubmissionBudget = 16 * 1024 * 1024;

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
            dispatch_semaphore_wait(inFlight, DISPATCH_TIME_FOREVER);
            commands = [queue commandBuffer];
            if (!commands) {
                dispatch_semaphore_signal(inFlight);
                throw std::runtime_error("Metal command buffer allocation failed");
            }
            // Capture shared completion state, never this (the callback can
            // outlive the C++ resource wrappers).
            dispatch_semaphore_t semaphore = inFlight;
            auto fault = gpuFailed;
            [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                if (buffer.status == MTLCommandBufferStatusError) {
                    fault->store(true, std::memory_order_relaxed);
                    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Metal GPU error: %s", buffer.error.localizedDescription.UTF8String);
                }
                dispatch_semaphore_signal(semaphore);
            }];
        }
        return commands;
    }
    bool Submit(bool wait = false)
    {
        if (commands) {
            lastSubmitted = commands;
            [commands commit];
            commands = nil;
            transientBytes = 0;
        }
        if (wait && lastSubmitted) {
            [lastSubmitted waitUntilCompleted];
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
        id<MTLBuffer> staging = [device newBufferWithLength:rowBytes * h options:MTLResourceStorageModeShared];
        if (!staging) throw std::runtime_error("Metal upload buffer allocation failed");
        for (int y = 0; y < h; ++y)
            std::memcpy(static_cast<uint8_t*>(staging.contents) + y * rowBytes,
                        pixels + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * 4);
        id<MTLBlitCommandEncoder> e = [Commands() blitCommandEncoder];
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
    bool Read(id<MTLTexture> texture, std::vector<uint8_t>& pixels, int& pitch)
    {
        pitch = 0;
        const size_t w = texture.width, h = texture.height;
        const size_t rowBytes = (w * 4 + 255) & ~size_t(255);
        id<MTLBuffer> staging = [device newBufferWithLength:rowBytes * h options:MTLResourceStorageModeShared];
        if (!staging) return false;
        id<MTLBlitCommandEncoder> e = [Commands() blitCommandEncoder];
        if (!e) return false;
        [e copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0
                destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * h];
        [e endEncoding];
        if (!Submit(true)) return false;
        pixels.resize(w * h * 4);
        pitch = static_cast<int>(w * 4);
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
        id<MTLBlitCommandEncoder> e = [Commands() blitCommandEncoder];
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
    id<MTLRenderPipelineState> Pipeline(id<MTLLibrary> lib, NSString* fragment,
                                       MTLPixelFormat format, int blend)
    {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vertexMain"];
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
        layer.maximumDrawableCount = 2;
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
        windowPipeline = Pipeline(lib, @"windowMain", MTLPixelFormatBGRA8Unorm, -1);
        capturePipeline = Pipeline(lib, @"windowMain", MTLPixelFormatRGBA8Unorm, -1);
        for (int i = 0; i < 3; ++i) meshPipelines[i] = Pipeline(lib, @"meshMain", MTLPixelFormatRGBA8Unorm, i);
        layerPipeline = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"layerMain"] error:&error];
        if (!layerPipeline) SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Metal layer pipeline: %s", error.localizedDescription.UTF8String);
        return windowPipeline && capturePipeline && meshPipelines[0] && meshPipelines[1] &&
               meshPipelines[2] && layerPipeline;
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
void MetalRenderBackend::BeginFrame(int w, int h)
{
    impl_->width = w; impl_->height = h;
    impl_->windows.clear();
}
void MetalRenderBackend::EndFrame()
{
    @autoreleasepool {
        auto& p = *impl_;
        if (p.width > 0 && p.height > 0 && !(SDL_GetWindowFlags(p.window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))) {
            p.layer.drawableSize = CGSizeMake(p.width, p.height);
            id<CAMetalDrawable> drawable = [p.layer nextDrawable];
            if (drawable) {
                p.DrawWindows(drawable.texture, p.windowPipeline);
                [p.Commands() presentDrawable:drawable];
            }
        }
        p.Submit(); // Offscreen work must still complete when no drawable is available.
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
        for (int i = 0; i < indexCount; ++i) if (indices[i] >= vertexCount) return;
        id<MTLTexture> source = r->texture;
        // Avoid sampling the render attachment, including when it is also the mask.
        id<MTLTexture> snapshot = (r == p.current || p.mask == p.current) ? p.Snapshot(p.current) : nil;
        if (r == p.current) source = snapshot;
        id<MTLBuffer> v = [p.device newBufferWithBytes:vertices length:static_cast<size_t>(vertexCount) * 4 * sizeof(float)
                                            options:MTLResourceStorageModeShared];
        id<MTLBuffer> i = [p.device newBufferWithBytes:indices length:static_cast<size_t>(indexCount) * sizeof(uint16_t)
                                            options:MTLResourceStorageModeShared];
        if (!v || !i) throw std::runtime_error("Metal mesh buffer allocation failed");
        Params uniforms = p.mesh;
        uniforms.modulation = modulation ? Color(modulation) : simd_make_float4(1, 1, 1, 1);
        uniforms.flags.y = p.mask != nullptr;
        uniforms.values = simd_make_float4(std::clamp(opacity, 0.0f, 1.0f), p.current->width, p.current->height, 0);
        int pipeline = (uniforms.flags.x == 1 || uniforms.flags.x == 4) ? 1 : uniforms.flags.x == 21 ? 2 : 0;
        id<MTLRenderCommandEncoder> e = p.Pass(p.current->texture, false);
        [e setRenderPipelineState:p.meshPipelines[pipeline]];
        [e setVertexBuffer:v offset:0 atIndex:0];
        [e setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [e setFragmentTexture:source atIndex:0];
        [e setFragmentTexture:p.mask ? (p.mask == p.current ? snapshot : p.mask->texture) : source atIndex:1];
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:indexCount indexType:MTLIndexTypeUInt16
                     indexBuffer:i indexBufferOffset:0];
        [e endEncoding];
        p.transientBytes += static_cast<size_t>(vertexCount) * 4 * sizeof(float) +
                            static_cast<size_t>(indexCount) * sizeof(uint16_t);
        if (p.transientBytes >= Impl::kSubmissionBudget) p.Submit();
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
        id<MTLComputeCommandEncoder> e = [p.Commands() computeCommandEncoder];
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
} // namespace krkrsdl3
