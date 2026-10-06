#pragma once
#include "LayerRenderOperation.h"
#include <string>
#include <memory>
class iTVPTexture2D;
namespace krkrsdl3 { struct AsyncAlphaTile; struct AsyncLayerPresentation; }
// UI preflight only. False means an ordinary/non-Metal texture; a null tile
// means the bounded async cache is busy, never permission for a sync read.
bool TVPRequestEmoteAsyncAlpha(iTVPTexture2D*, int x, int y,
                             std::shared_ptr<krkrsdl3::AsyncAlphaTile>&);
void TVPMarkEmoteAlphaTexture(iTVPTexture2D*);
bool TVPIsEmoteAsyncAlphaTexture(iTVPTexture2D*);
// Explicit window-composition snapshot. Ordinary texture handle acquisition
// (including script GPU writes) must never publish a displayed alpha version.
void TVPEncodeEmoteAsyncAlphaForPresentation(iTVPTexture2D*);
// The texture is an intrusive-ref-held immutable MainImage epoch from F. The
// copy may be encoded in a later normal command buffer, but its input/display
// version remains F rather than the current mutable Layer texture.
void TVPEncodeFrozenEmoteAsyncAlpha(iTVPTexture2D*,
    const std::shared_ptr<krkrsdl3::AsyncLayerPresentation>&);
void TVPStopEmoteAsyncAlphaDemand(iTVPTexture2D*);
uint64_t TVPGetEmoteAlphaPresentationSerial();
std::shared_ptr<krkrsdl3::AsyncLayerPresentation> TVPGetEmoteAlphaPresentation();
void TVPBeginEmoteAlphaPresentation();
void TVPEndEmoteAlphaPresentation();
namespace krkrsdl3 { class iTVPRenderBackend; }
// The facade and software methods have process lifetime; only the binding is
// session scoped. Unbind after clearing layers and recycling deleted textures.
bool TVPBindMetalLayerRenderManager(krkrsdl3::iTVPRenderBackend* backend);
void TVPUnbindMetalLayerRenderManager();
const char* TVPMetalLayerFallbackReason();
bool TVPMetalLayerCompositionActive();
TVPLayerRenderStats TVPGetMetalLayerRenderStats();
std::string TVPGetMetalLayerMultipleInputMethodSummary();
std::string TVPGetMetalLayerUnsupportedMethodSummary();

struct TVPLayerTriangleFallbackStats {
    uint64_t intervalNS = 0;
    uint64_t calls = 0, triangleCount = 0;
    // Successful Copy/blend affine calls and dispatched pixels, separate from fallback.
    uint64_t gpuCalls = 0, gpuPixels = 0;
    // Clip intersected with the target, not triangle-covered/rasterized pixels.
    uint64_t clipPixels = 0, maxClipPixels = 0, maxTargetPixels = 0;
    uint64_t fullSurfaceCalls = 0, target1920x1080Calls = 0;
    uint64_t targetReadbackBytes = 0, sourceReadbackBytes = 0, referenceReadbackBytes = 0;
    // Entire fallback wall time includes synchronous GPU readback; software
    // time measures just the software OperateTriangles call inside that time.
    uint64_t cpuTimeNS = 0, maxCpuTimeNS = 0;
    uint64_t softwareTimeNS = 0, maxSoftwareTimeNS = 0;
    uint64_t count2Calls = 0, singleInputCalls = 0, referenceCalls = 0, sourceTargetAliasCalls = 0;
};
struct TVPLayerTriangleProfile {
    TVPLayerTriangleFallbackStats stats;
    // Bounded interval histograms, "name:calls"; otherCalls includes omitted
    // entries so histogram counts still sum to stats.calls.
    std::string methods, targetSizes, sources, stretchModes;
};
// Call on the render/main thread. Only the log sampler takes/resets an interval;
// ordinary GetStats/HUD reads never consume it. Disabling drops pending samples.
void TVPSetMetalLayerTriangleDiagnostics(bool enabled);
TVPLayerTriangleProfile TVPTakeMetalLayerTriangleProfile();
