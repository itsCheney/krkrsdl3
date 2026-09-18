#pragma once

#include <memory>
#include "TVPCompositor.h"

struct SDL_Window;

namespace krkrsdl3
{
// Native Metal compositor and offscreen renderer. All calls run on the SDL main thread.
class MetalRenderBackend final : public iTVPRenderBackend
{
public:
    static iTVPRenderBackend* Create(SDL_Window* window, bool vsync);
    ~MetalRenderBackend() override;
    const char* GetName() const override { return "metal"; }
    bool IsHardware() const override { return true; }
    void FetchInfo() override;
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
