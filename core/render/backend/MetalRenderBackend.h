#pragma once

#include <memory>
#include "TVPCompositor.h"

struct SDL_Window;

namespace krkrsdl3
{
// Diagnostic metadata only: GPU durations belong to the whole command buffer.
// A buffer containing multiple kinds of work must never be presented as the
// duration of one of its stages, or divided according to draw/encoder counts.
namespace metal_diagnostics
{
struct Workload
{
    enum Stage : uint32_t { Mesh = 1, Layer = 2, Blit = 4, Window = 8, OtherRender = 16 };
    uint32_t stages = 0;
    uint32_t renderEncoders = 0, computeEncoders = 0, blitEncoders = 0;
    uint32_t meshDraws = 0, deformDraws = 0, maskedDraws = 0, clears = 0;
    uint32_t layerDispatches = 0, windowDraws = 0;
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
    bool SetLayerAlphaTables(const uint8_t*, const uint8_t*) override;
    void* CreateLayerTexture(int, int, TVPLayerTextureFormat) override;
    void DestroyLayerTexture(void*) override;
    bool UpdateLayerTexture(void*, const uint8_t*, int, const TVPLayerRect&) override;
    bool CopyTargetToLayerTexture(void*, void*) override;
    bool ReadLayerTexture(void*, std::vector<uint8_t>&, int&) override;
    bool ReadLayerTextureRegion(void*, const TVPLayerRect&, std::vector<uint8_t>&, int&) override;
    bool OperateLayerRect(const TVPLayerOperation&, void*, const TVPLayerRect&,
                          void*, const TVPLayerRect&, int) override;
    bool OperateLayerRectDualSource(const TVPLayerOperation&, void*, const TVPLayerRect&,
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
