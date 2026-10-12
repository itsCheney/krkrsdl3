#pragma once
#include "LayerRenderOperation.h"
#include <string>
#include <memory>
class iTVPTexture2D;
namespace krkrsdl3 { class iTVPRenderBackend; }
// The facade and software methods have process lifetime; only the binding is
// session scoped. Unbind after clearing layers and recycling deleted textures.
bool TVPBindMetalLayerRenderManager(krkrsdl3::iTVPRenderBackend* backend);
void TVPUnbindMetalLayerRenderManager();
const char* TVPMetalLayerFallbackReason();
bool TVPMetalLayerCompositionActive();
// Call once per frame after script/event work and before presenting. Closes
// script raw-pointer write leases and unpins textures whose raw pointer
// script has stopped requesting, so they return to GPU composition.
void TVPEndMetalLayerFrame();
// GPU composition pays per update rectangle; software keeps CPU-era regions.
// MIKAGE_METAL_LAYER_COALESCE_UPDATES=0 restores the uncoalesced regions.
bool TVPShouldCoalesceMetalLayerUpdates();
TVPLayerRenderStats TVPGetMetalLayerRenderStats();
enum class TVPLayerImageResult { Applied, Resource, CPUAccess, Geometry, BackendFailure };
bool TVPHasMetalLayerImageLUTSupport();
// Existing RGB gamma/LUT kernel, without a software registry singleton/setter.
// Rejection precedes target writes; exceptions are never replayed on CPU.
TVPLayerImageResult TVPTryMetalLayerImageLUT(iTVPTexture2D*,const TVPLayerRect&,
    const std::shared_ptr<const TVPLayerGammaLUT>&);
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
