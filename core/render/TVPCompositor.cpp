#include "tjsCommHead.h"
#include "TVPCompositor.h"
#include "TVPDebug.h"

#include <algorithm>
#include <atomic>
#include <vector>

//---------------------------------------------------------------------------
// 窗口贴图合成调度器
//
// 职责：
//   1. 维护当前渲染后端（见 backend/RenderBackend.h，GL/Vulkan/SW 等实现）
//   2. 维护参与合成的贴图列表（窗口 sprite + overlay）
//   3. 统一计算 letterbox 变换（scale/xPos/yPos），再交给后端绘制
//
// 历史说明：本文件原为 GL 直写实现（shader/纹理/绘制），
// GL 代码已迁移至 backend/GLRenderBackend.cpp，此处不再包含任何图形 API 调用。
//---------------------------------------------------------------------------
static std::vector<TVPSprite*> renderTexture;

namespace krkrsdl3
{
//---------------------------------------------------------------------------
// 后端注册表
// （函数内静态变量：避免跨编译单元的静态初始化顺序问题）
//---------------------------------------------------------------------------
static std::vector<TVPRenderBackendDesc>& GetRegistry()
{
    static std::vector<TVPRenderBackendDesc> registry;
    return registry;
}
static iTVPRenderBackend*& GetCurrentBackendRef()
{
    static iTVPRenderBackend* backend = nullptr;
    return backend;
}

namespace {
std::atomic<uint64_t> emoteCaptureCalls{0};
std::atomic<uint64_t> emoteCaptureCPUFallbacks{0};
std::atomic<uint64_t> emoteCaptureCPUBytes{0};
std::atomic<uint64_t> emoteCaptureGPUCopies{0};
std::atomic<uint64_t> emoteCaptureGPUBytes{0};
}

void TVPRecordEmoteCaptureCall() {
    emoteCaptureCalls.fetch_add(1, std::memory_order_relaxed);
}
void TVPRecordEmoteCaptureCPUFallback(uint64_t bytes) {
    emoteCaptureCPUFallbacks.fetch_add(1, std::memory_order_relaxed);
    emoteCaptureCPUBytes.fetch_add(bytes, std::memory_order_relaxed);
}
void TVPRecordEmoteCaptureGPUCopy(uint64_t bytes) {
    emoteCaptureGPUCopies.fetch_add(1, std::memory_order_relaxed);
    emoteCaptureGPUBytes.fetch_add(bytes, std::memory_order_relaxed);
}
TVPEmoteCaptureStats TVPGetEmoteCaptureStats() {
    TVPEmoteCaptureStats stats;
    stats.calls=emoteCaptureCalls.load(std::memory_order_relaxed);
    stats.cpuFallbacks=emoteCaptureCPUFallbacks.load(std::memory_order_relaxed);
    stats.cpuBytes=emoteCaptureCPUBytes.load(std::memory_order_relaxed);
    stats.gpuCopies=emoteCaptureGPUCopies.load(std::memory_order_relaxed);
    stats.gpuBytes=emoteCaptureGPUBytes.load(std::memory_order_relaxed);
    return stats;
}
void TVPResetEmoteCaptureStats() {
    emoteCaptureCalls.store(0, std::memory_order_relaxed);
    emoteCaptureCPUFallbacks.store(0, std::memory_order_relaxed);
    emoteCaptureCPUBytes.store(0, std::memory_order_relaxed);
    emoteCaptureGPUCopies.store(0, std::memory_order_relaxed);
    emoteCaptureGPUBytes.store(0, std::memory_order_relaxed);
}

void TVPRegisterRenderBackend(const TVPRenderBackendDesc& desc)
{
    if (!desc.name)
        return;
    auto& registry = GetRegistry();
    for (const auto& existing : registry)
    {
        if (std::string(existing.name) == desc.name)
            return; // 已注册，去重
    }
    registry.push_back(desc);
}

std::vector<std::string> TVPListRenderBackends()
{
    std::vector<std::string> result;
    for (const auto& desc : GetRegistry())
    {
        if (desc.probe && !desc.probe())
            continue;
        result.push_back(desc.name);
    }
    return result;
}

bool TVPRenderBackendAvailable(const std::string& name)
{
    for (const auto& desc : GetRegistry())
    {
        if (desc.name == name)
            return !desc.probe || desc.probe();
    }
    return false;
}

//---------------------------------------------------------------------------
// 全局当前后端
//---------------------------------------------------------------------------
void TVPSetRenderBackend(iTVPRenderBackend* backend)
{
    GetCurrentBackendRef() = backend;
}

iTVPRenderBackend* TVPGetRenderBackend()
{
    return GetCurrentBackendRef();
}

void TVPShutdownRenderBackend()
{
    delete GetCurrentBackendRef();
    GetCurrentBackendRef() = nullptr;
}

// 合成器全局信息采集（转发给当前后端，GL 后端输出厂商/版本/扩展日志）
void fetchGLInfo()
{
    iTVPRenderBackend* backend = TVPGetRenderBackend();
    if (backend)
        backend->FetchInfo();
}

//---------------------------------------------------------------------------
// 会话切换诊断：只在合成状态发生变化时记录一行，避免每帧刷日志。
// 用来区分"overlay 不在列表里"、"在列表但被判定不可见"和"窗口贴图本身为空"。
// reset = true 时清空上一次会话的状态，保证新游戏的第一行一定被记录。
//---------------------------------------------------------------------------
static void TVPReportCompositorFrame(int spriteCount,
                                     int overlayCandidates,
                                     int overlayDrawn,
                                     bool windowDrawn,
                                     bool reset = false)
{
    static int lastSpriteCount = -1;
    static int lastOverlayCandidates = -1;
    static int lastOverlayDrawn = -1;
    static int lastWindowDrawn = -1;
    if (reset)
    {
        lastSpriteCount = -1;
        lastOverlayCandidates = -1;
        lastOverlayDrawn = -1;
        lastWindowDrawn = -1;
        return;
    }
    if (spriteCount == lastSpriteCount && overlayCandidates == lastOverlayCandidates &&
        overlayDrawn == lastOverlayDrawn && (int)windowDrawn == lastWindowDrawn)
        return;
    lastSpriteCount = spriteCount;
    lastOverlayCandidates = overlayCandidates;
    lastOverlayDrawn = overlayDrawn;
    lastWindowDrawn = (int)windowDrawn;
    TVPAddImportantLog(ttstr(TJS_N("(info) Compositor: sprites ")) + ttstr(spriteCount) +
                       TJS_N(", overlay ") + ttstr(overlayDrawn) + TJS_N("/") +
                       ttstr(overlayCandidates) + TJS_N(" drawn, window ") +
                       ttstr((tjs_int)windowDrawn));
}

void TVPResetCompositorSessionState()
{
    renderTexture.clear();
    TVPResetEmoteCaptureStats();
    TVPReportCompositorFrame(0, 0, 0, false, true);
}

// 素材加入渲染
void TVPJoinTexture(TVPSprite* sp)
{
    if (!sp)
        return;
    if (std::find(renderTexture.begin(), renderTexture.end(), sp) == renderTexture.end())
        renderTexture.push_back(sp);
}

// 素材离开渲染
void TVPDepartTexture(TVPSprite* sp)
{
    renderTexture.erase(
        std::remove(renderTexture.begin(), renderTexture.end(), sp),
        renderTexture.end());
}

// 统一 letterbox 计算：保持宽高比地缩放并居中（所有后端共用同一套行为）
static void TVPCalcLetterbox(TVPSprite* sp, int winWidth, int winHeight)
{
    float currScale = std::min(((float)winWidth) / sp->width, ((float)winHeight) / sp->height);
    sp->scale = currScale;
    sp->xPos = (winWidth - currScale * sp->width) / 2.0f;
    sp->yPos = (winHeight - currScale * sp->height) / 2.0f;
}

void TVPRenderOnce(int winWidth, int winHeight)
{
    iTVPRenderBackend* backend = TVPGetRenderBackend();
    if (!backend)
        return;

    backend->BeginFrame(winWidth, winHeight);

    // 绘制 currentSprite
    TVPSprite* retSpr = KRKR_Get_Current_Sprite();
    if (retSpr && retSpr->texture)
    {
        TVPCalcLetterbox(retSpr, winWidth, winHeight);
        backend->DrawWindowTexture(retSpr->texture, retSpr->xPos, retSpr->yPos,
                                   retSpr->scale * retSpr->width, retSpr->scale * retSpr->height);
    }

    // 绘制 overlay
    int overlayCandidates = 0;
    int overlayDrawn = 0;
    for (auto texture : renderTexture)
    {
        if (texture->type == 2)
            overlayCandidates++;
        if (texture->isVisible && texture->type == 2 && texture->texture)
        {
            overlayDrawn++;
            TVPCalcLetterbox(texture, winWidth, winHeight);
            backend->DrawWindowTexture(texture->texture, texture->xPos, texture->yPos,
                                       texture->scale * texture->width, texture->scale * texture->height);
        }
    }
    TVPReportCompositorFrame((int)renderTexture.size(), overlayCandidates, overlayDrawn,
                             retSpr != nullptr && retSpr->texture != nullptr);

    backend->EndFrame();
}

// 创建素材
void TVPCreateTexture(TVPSprite& sp)
{
    iTVPRenderBackend* backend = TVPGetRenderBackend();
    if (!backend)
        return;
    sp.texture = backend->CreateWindowTexture(sp.width, sp.height);
}

// 更新素材
void TVPUpdateTexture(TVPSprite* sp, uint8_t* buff, int width, int height, int pitch)
{
    iTVPRenderBackend* backend = TVPGetRenderBackend();
    if (!backend || !sp->texture)
        return;
    backend->UpdateWindowTexture(sp->texture, buff, width, height, pitch);
}

// 销毁素材
void TVPDestroyTexture(TVPSprite* sp)
{
    iTVPRenderBackend* backend = TVPGetRenderBackend();
    // 借用的纹理（GPU 别名）不由 sprite 销毁，只解除引用
    if (backend && sp->texture && !sp->borrowedTexture)
        backend->DestroyWindowTexture(sp->texture);
    sp->texture = nullptr;
    sp->borrowedTexture = false;
}

// TODO 或许应该和window整合起来管理
void TVPClearAllTexture()
{
    for (auto sp : renderTexture)
    {
        TVPDestroyTexture(sp);
    }
    renderTexture.clear();
}

} // namespace krkrsdl3
