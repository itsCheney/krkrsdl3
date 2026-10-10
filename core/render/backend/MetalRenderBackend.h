#pragma once

#include <memory>
#include <array>
#include <string>
#include "TVPCompositor.h"

struct SDL_Window;

namespace krkrsdl3
{
// Diagnostic metadata only: GPU durations belong to the whole command buffer.
// A buffer containing multiple kinds of work must never be presented as the
// duration of one of its stages, or divided according to draw/encoder counts.
namespace metal_diagnostics
{
// The extension fixture uses this same storage, bounds check and formatter
// with an appended kind. Production storage always follows its actual Count.
template<std::size_t KindCount>
struct LayerKindPixelCounters
{
    std::array<uint64_t, KindCount> pixelsByKind{};
    void RecordKindPixels(int kind, uint64_t pixels) {
        if(kind >= 0 && static_cast<std::size_t>(kind) < pixelsByKind.size())
            pixelsByKind[static_cast<std::size_t>(kind)] += pixels;
    }
    std::string KindPixelSummary() const {
        std::string out;
        for(std::size_t kind=0;kind<pixelsByKind.size();++kind) if(pixelsByKind[kind]) {
            if(!out.empty()) out+=',';
            out+=std::to_string(kind)+":"+std::to_string(pixelsByKind[kind]);
        }
        return out;
    }
};
struct Workload : LayerKindPixelCounters<TVP_LAYER_OPERATION_COUNT>
{
    enum Stage : uint32_t { Mesh = 1, Layer = 2, Blit = 4, Window = 8, OtherRender = 16 };
    uint32_t stages = 0;
    uint32_t renderEncoders = 0, computeEncoders = 0, blitEncoders = 0;
    uint32_t meshDraws = 0, deformDraws = 0, maskedDraws = 0, clears = 0;
    uint32_t layerDispatches = 0, windowDraws = 0;
    uint32_t rectCalls=0, tileDraws=0;
    uint64_t rectPixels=0, scaledPixels=0, aliasPixels=0, blurPixels=0;
    void Rect(int kind,uint64_t pixels,bool scaled,bool alias,bool tile) {
        ++rectCalls; tileDraws+=tile; rectPixels+=pixels;
        if(scaled) scaledPixels+=pixels; if(alias) aliasPixels+=pixels;
        if(kind==static_cast<int>(TVPLayerOperationKind::BoxBlur)) blurPixels+=pixels;
        RecordKindPixels(kind,pixels);
    }
    const char* Bucket() const
    {
        switch (stages) {
            case 0: return "empty";
            case Mesh: return "mesh_or_clear";
            case Layer: return "layer_compute";
            case Blit: return "blit";
            case Window: return "window_render";
            case OtherRender: return "other_render";
            default: return "mixed";
        }
    }
};
struct Sampler
{
    uint64_t nextSampleNS = 0;
    bool ShouldSample(bool enabled, uint64_t nowNS)
    {
        if (!enabled || nowNS < nextSampleNS) return false;
        nextSampleNS = nowNS + 1000000000ULL;
        return true;
    }
};
}

// Native Metal compositor and offscreen renderer. All calls run on the SDL main thread.
class MetalRenderBackend final : public iTVPRenderBackend
{
public:
    static iTVPRenderBackend* Create(SDL_Window* window, bool vsync);
    ~MetalRenderBackend() override;
    const char* GetName() const override { return "metal"; }
    bool IsHardware() const override { return true; }
    void FetchInfo() override;
    bool SupportsLayerOperations() const override;
    bool SupportsLayerTransitions() const override;
    bool OperateLayerTransition(const TVPLayerTransitionOperation&, void*, void*, void*) override;
    TVPLayerTransitionResult LastLayerTransitionResult() const override;
    bool SupportsLayerShrinks() const override;
    bool SupportsLayerShrink64() const override;
    bool OperateLayerShrink(const TVPLayerShrinkOperation&, void*, void*) override;
    TVPLayerShrinkResult LastLayerShrinkResult() const override;
    bool SupportsLayerSpanComposition() const override;
    bool OperateLayerSpanComposite(const TVPLayerSpanCompositePacket&,void*) override;
    TVPLayerSpanCompositeResult LastLayerSpanCompositeResult() const override;
    TVPLayerParameterUploadStats GetLayerParameterUploadStats() const override;
    bool SupportsLayerTileRendering() const;
    bool IsLayerTileRenderingActive() const;
    bool SetLayerAlphaTables(const uint8_t*, const uint8_t*) override;
    bool SetLayerPsTables(const uint8_t*, const uint8_t*, const uint8_t*) override;
    void* CreateLayerTexture(int, int, TVPLayerTextureFormat) override;
    void SetLayerDiagnosticIdentity(void*,const layer_hotspot::Identity&) override;
    void DestroyLayerTexture(void*) override;
    bool UpdateLayerTexture(void*, const uint8_t*, int, const TVPLayerRect&) override;
    bool OperateLayerGlyph(const TVPLayerOperation&,void*,const TVPLayerRect&,
        const uint8_t*,int,int,int,int,TVPLayerGlyphUploadInfo&) override;
    void ResetLayerGlyphResources() override;
    void RecordLayerGlyphRejection(layer_upload::GlyphReject) override;
    bool CopyTargetToLayerTexture(void*, void*) override;
    bool CopyTargetToLayerTextureRegion(void*, void*, const TVPLayerRect&) override;
    bool ReadLayerTexture(void*, std::vector<uint8_t>&, int&) override;
    bool ReadLayerTextureRegion(void*, const TVPLayerRect&, std::vector<uint8_t>&, int&) override;
    uint64_t GetLastReadbackWaitNanoseconds() const override;
    std::shared_ptr<AsyncLayerPresentation> GetCurrentLayerPresentation() const override;
    uint64_t GetLastLayerPresentationSerial() const override;
    bool RequestLayerTextureRegionRead(void*, const TVPLayerRect&,
                                      const std::shared_ptr<AsyncLayerReadback>&) override;
    bool OperateLayerRect(const TVPLayerOperation&, void*, const TVPLayerRect&,
                          void*, const TVPLayerRect&, int) override;
    bool OperateLayerAffine(const TVPLayerOperation&, void*, const TVPLayerAffineCopy&,
                            void*, int) override;
    bool OperateLayerPerspective(const TVPLayerOperation&, void*, const TVPLayerPerspectiveQuad*,
                                 size_t, void*, int) override;
    bool OperateLayerRectDualSource(const TVPLayerOperation&, void*, const TVPLayerRect&,
                                    void*, const TVPLayerRect&,
                                    void*, const TVPLayerRect&) override;
    bool OperateLayerRectTripleSource(const TVPLayerOperation&, void*, const TVPLayerRect&,
                                      void*, const TVPLayerRect&,
                                      void*, const TVPLayerRect&,
                                      void*, const TVPLayerRect&) override;
    double GetGpuSubmissionTimeMilliseconds() const override;
    double GetPresentationWaitTimeMilliseconds() const override;
    void BeginFrame(int width, int height) override;
    void EndFrame() override;
    void* CreateWindowTexture(int width, int height) override;
    void UpdateWindowTexture(void*, const uint8_t*, int, int, int) override;
    void DestroyWindowTexture(void*) override;
    void DrawWindowTexture(void*, float, float, float, float) override;
    void* CreateTarget(int width, int height) override;
    void DestroyTarget(void*) override;
    void SetTarget(void*) override;
    void ClearTarget(bool clearColor) override;
    uint8_t* LockTarget(void*, int& pitch) override;
    void UnlockTarget(void*) override;
    void* GetTargetTexture(void*) override;
    void UpdateTargetTexture(void*, const uint8_t*, int, int, int) override;
    void* CreateTexture(int width, int height) override;
    void UpdateTexture(void*, const uint8_t*, int, int, int) override;
    void DestroyTexture(void*) override;
    void SetMask(void*) override;
    void SetBlendMode(int mode, const float* color) override;
    void DrawMesh(const float*, int, const uint16_t*, int, void*, float,
                  const float* colorModulation = nullptr) override;
    bool SupportsMeshDeformation() const override { return true; }
    bool DrawDeformedMesh(int, int, const TVPMeshDeformSurface*, int, void*, float,
                          const float* colorModulation = nullptr) override;
    void LayerSetBlend(int method, float opacity, const float* color) override;
    void LayerDrawRect(void*, float, float, float, float, float, float, float, float) override;
    bool CaptureFrame(std::vector<uint8_t>& pixels, int& width, int& height, int& pitch) override;

private:
    struct Impl;
    MetalRenderBackend();
    std::unique_ptr<Impl> impl_;
};
bool MetalRenderBackendAvailable();
} // namespace krkrsdl3
