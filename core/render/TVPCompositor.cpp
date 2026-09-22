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

std::atomic<uint64_t> profileEmoteProgressCalls{0}, profileEmoteProgressTimeNS{0};
std::atomic<uint64_t> profileEmotePrepareCalls{0}, profileEmotePrepareTimeNS{0};
std::atomic<uint64_t> profileEmoteDrawCalls{0}, profileEmoteDrawTimeNS{0};
std::atomic<uint64_t> profileEmoteCaptureCalls{0}, profileEmoteCaptureTimeNS{0};
std::atomic<uint64_t> profileEmotePrepareTransformTimeNS{0};
std::atomic<uint64_t> profileEmotePrepareMotionProgressTimeNS{0};
std::atomic<uint64_t> profileEmotePrepareSnapshotTimeNS{0};
std::atomic<uint64_t> profileEmoteNodeProgressCalls{0}, profileEmoteNodeProgressTimeNS{0};
std::atomic<uint64_t> profileEmoteSubmotionCreates{0}, profileEmoteSubmotionRebuildTimeNS{0};
std::atomic<uint64_t> profileEmoteShapeBuildCalls{0}, profileEmoteShapeBuildTimeNS{0};
std::atomic<uint64_t> profileEmoteShapeVertices{0};
std::atomic<uint64_t> profileEmoteMeshBuildCalls{0}, profileEmoteMeshBuildTimeNS{0};
std::atomic<uint64_t> profileEmoteMeshVerticesBuilt{0}, profileEmoteDeformedVerticesBuilt{0};

struct EmotePrepareDetailAccumulator {
    bool active = false;
    uint64_t nodeCalls = 0;
    uint64_t nodeTimeNS = 0;
    uint64_t submotionCreates = 0;
    uint64_t submotionRebuildTimeNS = 0;
    uint64_t shapeBuildCalls = 0;
    uint64_t shapeBuildTimeNS = 0;
    uint64_t shapeVertices = 0;
    uint64_t meshBuildCalls = 0;
    uint64_t meshBuildTimeNS = 0;
    uint64_t meshVertices = 0;
    uint64_t deformedVertices = 0;
};
thread_local EmotePrepareDetailAccumulator emotePrepareDetail;

std::atomic<uint64_t> profileMeshDrawCalls{0}, profileMeshVertices{0}, profileMeshIndices{0};
std::atomic<uint64_t> profileMeshCPUTimeNS{0}, profileMeshValidationTimeNS{0};
std::atomic<uint64_t> profileMeshBufferAllocations{0}, profileMeshBufferBytes{0};
std::atomic<uint64_t> profileMeshBufferAllocationTimeNS{0};
std::atomic<uint64_t> profileMetalSubmits{0}, profileMetalSyncWaits{0};
std::atomic<uint64_t> profileMetalSyncWaitTimeNS{0}, profileMetalQueueWaitTimeNS{0};
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

void TVPRecordEmoteProgress(uint64_t ns) {
    profileEmoteProgressCalls.fetch_add(1,std::memory_order_relaxed);
    profileEmoteProgressTimeNS.fetch_add(ns,std::memory_order_relaxed);
}
void TVPRecordEmotePrepare(uint64_t ns) {
    profileEmotePrepareCalls.fetch_add(1,std::memory_order_relaxed);
    profileEmotePrepareTimeNS.fetch_add(ns,std::memory_order_relaxed);
}
void TVPRecordEmoteDraw(uint64_t ns) {
    profileEmoteDrawCalls.fetch_add(1,std::memory_order_relaxed);
    profileEmoteDrawTimeNS.fetch_add(ns,std::memory_order_relaxed);
}
void TVPRecordEmoteCaptureTime(uint64_t ns) {
    profileEmoteCaptureCalls.fetch_add(1,std::memory_order_relaxed);
    profileEmoteCaptureTimeNS.fetch_add(ns,std::memory_order_relaxed);
}

void TVPBeginEmotePrepareDetail() {
    emotePrepareDetail = {};
    emotePrepareDetail.active = true;
}
void TVPRecordEmoteNodeProgress(uint64_t ns) {
    if (!emotePrepareDetail.active) return;
    ++emotePrepareDetail.nodeCalls;
    emotePrepareDetail.nodeTimeNS += ns;
}
void TVPRecordEmoteSubmotionRebuild(uint64_t ns, uint64_t creations) {
    if (!emotePrepareDetail.active) return;
    emotePrepareDetail.submotionCreates += creations;
    emotePrepareDetail.submotionRebuildTimeNS += ns;
}
void TVPRecordEmoteShapeBuild(uint64_t ns, uint64_t vertices) {
    if (!emotePrepareDetail.active) return;
    ++emotePrepareDetail.shapeBuildCalls;
    emotePrepareDetail.shapeBuildTimeNS += ns;
    emotePrepareDetail.shapeVertices += vertices;
}
void TVPRecordEmoteMeshBuild(uint64_t ns, uint64_t vertices, uint64_t deformedVertices) {
    if (!emotePrepareDetail.active) return;
    ++emotePrepareDetail.meshBuildCalls;
    emotePrepareDetail.meshBuildTimeNS += ns;
    emotePrepareDetail.meshVertices += vertices;
    emotePrepareDetail.deformedVertices += deformedVertices;
}
void TVPCommitEmotePrepareDetail(uint64_t transformNS, uint64_t motionNS, uint64_t snapshotNS) {
    if (!emotePrepareDetail.active) return;
    profileEmotePrepareTransformTimeNS.fetch_add(transformNS,std::memory_order_relaxed);
    profileEmotePrepareMotionProgressTimeNS.fetch_add(motionNS,std::memory_order_relaxed);
    profileEmotePrepareSnapshotTimeNS.fetch_add(snapshotNS,std::memory_order_relaxed);
    profileEmoteNodeProgressCalls.fetch_add(emotePrepareDetail.nodeCalls,std::memory_order_relaxed);
    profileEmoteNodeProgressTimeNS.fetch_add(emotePrepareDetail.nodeTimeNS,std::memory_order_relaxed);
    profileEmoteSubmotionCreates.fetch_add(emotePrepareDetail.submotionCreates,std::memory_order_relaxed);
    profileEmoteSubmotionRebuildTimeNS.fetch_add(emotePrepareDetail.submotionRebuildTimeNS,std::memory_order_relaxed);
    profileEmoteShapeBuildCalls.fetch_add(emotePrepareDetail.shapeBuildCalls,std::memory_order_relaxed);
    profileEmoteShapeBuildTimeNS.fetch_add(emotePrepareDetail.shapeBuildTimeNS,std::memory_order_relaxed);
    profileEmoteShapeVertices.fetch_add(emotePrepareDetail.shapeVertices,std::memory_order_relaxed);
    profileEmoteMeshBuildCalls.fetch_add(emotePrepareDetail.meshBuildCalls,std::memory_order_relaxed);
    profileEmoteMeshBuildTimeNS.fetch_add(emotePrepareDetail.meshBuildTimeNS,std::memory_order_relaxed);
    profileEmoteMeshVerticesBuilt.fetch_add(emotePrepareDetail.meshVertices,std::memory_order_relaxed);
    profileEmoteDeformedVerticesBuilt.fetch_add(emotePrepareDetail.deformedVertices,std::memory_order_relaxed);
    emotePrepareDetail.active = false;
}

void TVPRecordMeshDraw(uint64_t vertices,uint64_t indices,uint64_t cpuTimeNS,
                       uint64_t validationTimeNS,uint64_t bufferAllocations,
                       uint64_t bufferBytes,uint64_t bufferAllocationTimeNS) {
    profileMeshDrawCalls.fetch_add(1,std::memory_order_relaxed);
    profileMeshVertices.fetch_add(vertices,std::memory_order_relaxed);
    profileMeshIndices.fetch_add(indices,std::memory_order_relaxed);
    profileMeshCPUTimeNS.fetch_add(cpuTimeNS,std::memory_order_relaxed);
    profileMeshValidationTimeNS.fetch_add(validationTimeNS,std::memory_order_relaxed);
    profileMeshBufferAllocations.fetch_add(bufferAllocations,std::memory_order_relaxed);
    profileMeshBufferBytes.fetch_add(bufferBytes,std::memory_order_relaxed);
    profileMeshBufferAllocationTimeNS.fetch_add(bufferAllocationTimeNS,std::memory_order_relaxed);
}
void TVPRecordMetalSubmit() { profileMetalSubmits.fetch_add(1,std::memory_order_relaxed); }
void TVPRecordMetalSyncWait(uint64_t ns) {
    profileMetalSyncWaits.fetch_add(1,std::memory_order_relaxed);
    profileMetalSyncWaitTimeNS.fetch_add(ns,std::memory_order_relaxed);
}
void TVPRecordMetalQueueWait(uint64_t ns) {
    profileMetalQueueWaitTimeNS.fetch_add(ns,std::memory_order_relaxed);
}
TVPRuntimeProfileStats TVPGetRuntimeProfileStats() {
    TVPRuntimeProfileStats s;
    s.emoteProgressCalls=profileEmoteProgressCalls.load(std::memory_order_relaxed);
    s.emoteProgressTimeNS=profileEmoteProgressTimeNS.load(std::memory_order_relaxed);
    s.emotePrepareCalls=profileEmotePrepareCalls.load(std::memory_order_relaxed);
    s.emotePrepareTimeNS=profileEmotePrepareTimeNS.load(std::memory_order_relaxed);
    s.emoteDrawCalls=profileEmoteDrawCalls.load(std::memory_order_relaxed);
    s.emoteDrawTimeNS=profileEmoteDrawTimeNS.load(std::memory_order_relaxed);
    s.emoteCaptureProfileCalls=profileEmoteCaptureCalls.load(std::memory_order_relaxed);
    s.emoteCaptureTimeNS=profileEmoteCaptureTimeNS.load(std::memory_order_relaxed);
    s.emotePrepareTransformTimeNS=profileEmotePrepareTransformTimeNS.load(std::memory_order_relaxed);
    s.emotePrepareMotionProgressTimeNS=profileEmotePrepareMotionProgressTimeNS.load(std::memory_order_relaxed);
    s.emotePrepareSnapshotTimeNS=profileEmotePrepareSnapshotTimeNS.load(std::memory_order_relaxed);
    s.emoteNodeProgressCalls=profileEmoteNodeProgressCalls.load(std::memory_order_relaxed);
    s.emoteNodeProgressTimeNS=profileEmoteNodeProgressTimeNS.load(std::memory_order_relaxed);
    s.emoteSubmotionCreates=profileEmoteSubmotionCreates.load(std::memory_order_relaxed);
    s.emoteSubmotionRebuildTimeNS=profileEmoteSubmotionRebuildTimeNS.load(std::memory_order_relaxed);
    s.emoteShapeBuildCalls=profileEmoteShapeBuildCalls.load(std::memory_order_relaxed);
    s.emoteShapeBuildTimeNS=profileEmoteShapeBuildTimeNS.load(std::memory_order_relaxed);
    s.emoteShapeVertices=profileEmoteShapeVertices.load(std::memory_order_relaxed);
    s.emoteMeshBuildCalls=profileEmoteMeshBuildCalls.load(std::memory_order_relaxed);
    s.emoteMeshBuildTimeNS=profileEmoteMeshBuildTimeNS.load(std::memory_order_relaxed);
    s.emoteMeshVerticesBuilt=profileEmoteMeshVerticesBuilt.load(std::memory_order_relaxed);
    s.emoteDeformedVerticesBuilt=profileEmoteDeformedVerticesBuilt.load(std::memory_order_relaxed);
    s.meshDrawCalls=profileMeshDrawCalls.load(std::memory_order_relaxed);
    s.meshVertices=profileMeshVertices.load(std::memory_order_relaxed);
    s.meshIndices=profileMeshIndices.load(std::memory_order_relaxed);
    s.meshCPUTimeNS=profileMeshCPUTimeNS.load(std::memory_order_relaxed);
    s.meshValidationTimeNS=profileMeshValidationTimeNS.load(std::memory_order_relaxed);
    s.meshBufferAllocations=profileMeshBufferAllocations.load(std::memory_order_relaxed);
    s.meshBufferBytes=profileMeshBufferBytes.load(std::memory_order_relaxed);
    s.meshBufferAllocationTimeNS=profileMeshBufferAllocationTimeNS.load(std::memory_order_relaxed);
    s.metalSubmits=profileMetalSubmits.load(std::memory_order_relaxed);
    s.metalSyncWaits=profileMetalSyncWaits.load(std::memory_order_relaxed);
    s.metalSyncWaitTimeNS=profileMetalSyncWaitTimeNS.load(std::memory_order_relaxed);
    s.metalQueueWaitTimeNS=profileMetalQueueWaitTimeNS.load(std::memory_order_relaxed);
    return s;
}
void TVPResetRuntimeProfileStats() {
    profileEmoteProgressCalls.store(0,std::memory_order_relaxed);
    profileEmoteProgressTimeNS.store(0,std::memory_order_relaxed);
    profileEmotePrepareCalls.store(0,std::memory_order_relaxed);
    profileEmotePrepareTimeNS.store(0,std::memory_order_relaxed);
    profileEmoteDrawCalls.store(0,std::memory_order_relaxed);
    profileEmoteDrawTimeNS.store(0,std::memory_order_relaxed);
    profileEmoteCaptureCalls.store(0,std::memory_order_relaxed);
    profileEmoteCaptureTimeNS.store(0,std::memory_order_relaxed);
    profileEmotePrepareTransformTimeNS.store(0,std::memory_order_relaxed);
    profileEmotePrepareMotionProgressTimeNS.store(0,std::memory_order_relaxed);
    profileEmotePrepareSnapshotTimeNS.store(0,std::memory_order_relaxed);
    profileEmoteNodeProgressCalls.store(0,std::memory_order_relaxed);
    profileEmoteNodeProgressTimeNS.store(0,std::memory_order_relaxed);
    profileEmoteSubmotionCreates.store(0,std::memory_order_relaxed);
    profileEmoteSubmotionRebuildTimeNS.store(0,std::memory_order_relaxed);
    profileEmoteShapeBuildCalls.store(0,std::memory_order_relaxed);
    profileEmoteShapeBuildTimeNS.store(0,std::memory_order_relaxed);
    profileEmoteShapeVertices.store(0,std::memory_order_relaxed);
    profileEmoteMeshBuildCalls.store(0,std::memory_order_relaxed);
    profileEmoteMeshBuildTimeNS.store(0,std::memory_order_relaxed);
    profileEmoteMeshVerticesBuilt.store(0,std::memory_order_relaxed);
    profileEmoteDeformedVerticesBuilt.store(0,std::memory_order_relaxed);
    emotePrepareDetail = {};
    profileMeshDrawCalls.store(0,std::memory_order_relaxed);
    profileMeshVertices.store(0,std::memory_order_relaxed);
    profileMeshIndices.store(0,std::memory_order_relaxed);
    profileMeshCPUTimeNS.store(0,std::memory_order_relaxed);
    profileMeshValidationTimeNS.store(0,std::memory_order_relaxed);
    profileMeshBufferAllocations.store(0,std::memory_order_relaxed);
    profileMeshBufferBytes.store(0,std::memory_order_relaxed);
    profileMeshBufferAllocationTimeNS.store(0,std::memory_order_relaxed);
    profileMetalSubmits.store(0,std::memory_order_relaxed);
    profileMetalSyncWaits.store(0,std::memory_order_relaxed);
    profileMetalSyncWaitTimeNS.store(0,std::memory_order_relaxed);
    profileMetalQueueWaitTimeNS.store(0,std::memory_order_relaxed);
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
    TVPResetRuntimeProfileStats();
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
