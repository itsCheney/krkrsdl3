#include "DrawDeviceD3D.h"

#include "Platform.h"
#include "D3DEmotePlayer.h"
#include "LayerManager.h"
#include "TVPWindow.h"
#include "tjsNativeLayer.h"
#include "TVPSystem.h"
#include "TVPDebug.h"
#include "RenderManager.h"

#include <algorithm>
#include <cmath>
#include <typeinfo>


//---------------------------------------------------------------------------
// DrawDeviceD3D 实现
//---------------------------------------------------------------------------
namespace drawdevice
{

// D3D picture blend for a Layer.type: the LayerDrawRect method used when the
// backend has no ordinary Layer operations, and the mesh mode for transformed
// pictures. Types without an exact equivalent there approximate alpha.
struct PictureBlend
{
    int lbm;
    int mesh;
};
static PictureBlend PictureBlendFor(int type, int opacity)
{
    using B = krkrsdl3::iTVPRenderBackend;
    switch (type)
    {
        case 0: // ltBinder
        case 1: // ltOpaque
            return {opacity >= 255 ? B::LBM_COPY : B::LBM_CONSTALPHA, B::MBM_LAYER_COPY};
        case 3: // ltAdditive
            return {B::LBM_ADD, B::MBM_LAYER_ADD};
        case 4: // ltSubtractive
            return {B::LBM_SUB, B::MBM_LAYER_SUB};
        case 5: // ltMultiplicative
            return {B::LBM_MUL_HDA, B::MBM_LAYER_MUL};
        case 12: // ltAddAlpha
            return {B::LBM_ALPHA, B::MBM_LAYER_ADDALPHA};
        default: // ltAlpha and the rest
            return {B::LBM_ALPHA, 0};
    }
}

// Layer 合成方法 → 2D 网格混合模式（仿射四边形走 DrawMesh 时使用）
static int LayerToMeshBlendMode(int lbm)
{
    using namespace krkrsdl3;
    switch (lbm)
    {
        case iTVPRenderBackend::LBM_COPY:
        case iTVPRenderBackend::LBM_CONSTALPHA:
            return iTVPRenderBackend::MBM_LAYER_COPY;
        case iTVPRenderBackend::LBM_ADD:
            return iTVPRenderBackend::MBM_LAYER_ADD;
        case iTVPRenderBackend::LBM_SUB:
            return iTVPRenderBackend::MBM_LAYER_SUB;
        case iTVPRenderBackend::LBM_MUL:
        case iTVPRenderBackend::LBM_MUL_HDA:
            return iTVPRenderBackend::MBM_LAYER_MUL;
        default:
            return 0;
    }
}

// Draws a picture with the software Layer formula for its type, exactly as a
// Layer would blend it. Only integer placements qualify, and backends without
// ordinary Layer operations refuse, leaving the approximate paths.
static bool OperatePictureRect(krkrsdl3::iTVPRenderBackend* backend, void* target, int targetW,
                               int targetH, D3DPicture* pic, int x, int y)
{
    int type = pic->blendMode == ltBinder ? ltOpaque : pic->blendMode;
    tTVPBBBltMethod method;
    if (!TVPLayerTypeToBltMethod((tTVPLayerType)type, pic->opacity, true, method))
        return false;
    iTVPRenderMethod* rmethod = TVPGetRenderManager()->GetRenderMethod(pic->opacity, true, method);
    TVPLayerOperation op;
    if (!rmethod || !rmethod->DescribeGpuOperation(op) || op.kind == TVPLayerOperationKind::Unsupported)
        return false;
    TVPLayerRect src{std::max(0, pic->SrcX), std::max(0, pic->SrcY),
                     std::min((int)pic->Image->Width, pic->SrcX + pic->SrcW),
                     std::min((int)pic->Image->Height, pic->SrcY + pic->SrcH)};
    x += src.left - pic->SrcX;
    y += src.top - pic->SrcY;
    TVPLayerRect dst{x, y, x + src.Width(), y + src.Height()};
    if (dst.left < 0) { src.left -= dst.left; dst.left = 0; }
    if (dst.top < 0) { src.top -= dst.top; dst.top = 0; }
    if (dst.right > targetW) { src.right -= dst.right - targetW; dst.right = targetW; }
    if (dst.bottom > targetH) { src.bottom -= dst.bottom - targetH; dst.bottom = targetH; }
    if (src.Width() <= 0 || src.Height() <= 0)
        return true; // nothing visible
    return backend->OperateLayerRect(op, target, dst, pic->Image->Texture, src, 0);
}

// ===========================================================================
// 构造 / 析构
// ===========================================================================
DrawDeviceD3D::DrawDeviceD3D(tjs_int w, tjs_int h) : Width(w), Height(h)
{
    ScreenW = w;
    ScreenH = h;
    EnsureBackend();
    TVPAddImportantLog(ttstr("DrawDevice: DrawDeviceD3D created (") + ttstr(w) + TJS_N("x") + ttstr(h) + TJS_N(")"));

    // 不切换全局渲染管理器：Layer 树由当前 RenderManager 合成（Metal 后端时
    // 为 GPU Layer 合成），DrawBuffer 驻留 GPU 时直接作为贴图合成。
}

DrawDeviceD3D::~DrawDeviceD3D()
{
    // 解除窗口 sprite 对合成目标纹理的借用（由窗口侧管理，不再直接操作 sprite）；
    // Window 析构时已经 SetWindowInterface(nullptr) 解除引用，此处安全
    if (Window)
        Window->ReleaseBorrowedTexture();
    ReleaseBackendResources();
    for (auto& mi : Managers)
    {
        if (mi.Manager)
        {
            if (auto* manager = dynamic_cast<tTVPLayerManager*>(mi.Manager))
            {
                manager->SetPointerPresentationHitTestingEnabled(true);
                manager->SetHoldAlpha(true);
            }
            mi.Manager->Release();
        }
    }
    Managers.clear();
}

void DrawDeviceD3D::Destruct()
{
    delete this;
}

// Every handle is destroyed through the backend that created it.
void DrawDeviceD3D::ReleaseBackendResources()
{
    if (Backend)
    {
        if (PresentScratchTexture)
            Backend->DestroyTexture(PresentScratchTexture);
        if (CompositeTarget)
            Backend->DestroyTarget(CompositeTarget);
        if (PrevCompositeTarget)
            Backend->DestroyTarget(PrevCompositeTarget);
        if (ScratchTexture)
            Backend->DestroyTexture(ScratchTexture);
    }
    PresentScratchTexture = nullptr;
    CompositeTarget = nullptr;
    PrevCompositeTarget = nullptr;
    ScratchTexture = nullptr;
    ScratchW = ScratchH = 0;
}

void DrawDeviceD3D::EnsureBackend()
{
    if (!Backend)
        Backend = krkrsdl3::TVPGetRenderBackend();
}

bool DrawDeviceD3D::IsSoftwareBackend() const
{
    return Backend && std::string(Backend->GetName()) == "software";
}

// ===========================================================================
// iTVPDrawDevice —— 窗口 / 输入转发
// ===========================================================================
void DrawDeviceD3D::SetWindowInterface(TVPWindow* window)
{
    Window = window;
    UpdatePointerPresentationHitTesting();
}

void DrawDeviceD3D::UpdatePointerPresentationHitTesting()
{
    // Native subclasses may override cursor/hint callbacks. Only the two
    // built-in implementations are known to forward to TVPWindow's sinks.
    const bool builtIn = typeid(*this) == typeid(DrawDeviceD3D) || typeid(*this) == typeid(D3D);
    for (auto& mi : Managers)
    {
        if (auto* manager = dynamic_cast<tTVPLayerManager*>(mi.Manager))
        {
            const bool noPresentation = builtIn && Window &&
                typeid(*Window) == typeid(TVPWindow) &&
                manager->GetLayerTreeOwner() == static_cast<iTVPLayerTreeOwner*>(Window) &&
                !Window->HasNativePointerPresentation();
            manager->SetPointerPresentationHitTestingEnabled(!noPresentation);
        }
    }
}

void DrawDeviceD3D::AddLayerManager(iTVPLayerManager* manager)
{
    ManagerInfo info;
    info.Manager = manager;
    info.Primary = manager->GetPrimaryLayer();
    manager->AddRef();
    manager->SetDesiredLayerType(ltOpaque);
    Managers.push_back(info);
    UpdatePointerPresentationHitTesting();
}

void DrawDeviceD3D::RemoveLayerManager(iTVPLayerManager* manager)
{
    for (auto it = Managers.begin(); it != Managers.end(); ++it)
    {
        if (it->Manager == manager)
        {
            if (auto* concrete = dynamic_cast<tTVPLayerManager*>(it->Manager))
            {
                concrete->SetPointerPresentationHitTestingEnabled(true);
                concrete->SetHoldAlpha(true);
            }
            it->Manager->Release();
            Managers.erase(it);
            break;
        }
    }
}

void DrawDeviceD3D::GetSrcSize(tjs_int& w, tjs_int& h)
{
    w = Width;
    h = Height;
}

void DrawDeviceD3D::NotifyLayerResize(iTVPLayerManager* manager)
{
    if (Window)
        Window->NotifySrcResize();
}

void DrawDeviceD3D::NotifyLayerImageChange(iTVPLayerManager* manager)
{
    // 层内容变化 → 请求窗口更新（与 tTVPBasicDrawDevice 行为一致），
    // 否则 UpdateContent 不会触发，画面不会刷新
    if (Window && !RequestingComposition)
        Window->RequestUpdate();
}

void DrawDeviceD3D::OnClick(tjs_int x, tjs_int y)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyClick(x, y);
}
void DrawDeviceD3D::OnDoubleClick(tjs_int x, tjs_int y)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyDoubleClick(x, y);
}
void DrawDeviceD3D::OnMouseDown(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyMouseDown(x, y, mb, flags);
}
void DrawDeviceD3D::OnMouseUp(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyMouseUp(x, y, mb, flags);
}
void DrawDeviceD3D::OnMouseMove(tjs_int x, tjs_int y, tjs_uint32 flags)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyMouseMove(x, y, flags);
}
void DrawDeviceD3D::OnReleaseCapture()
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->ReleaseCapture();
}
void DrawDeviceD3D::OnMouseOutOfWindow()
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyMouseOutOfWindow();
}
void DrawDeviceD3D::OnKeyDown(tjs_uint key, tjs_uint32 shift)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyKeyDown(key, shift);
}
void DrawDeviceD3D::OnKeyUp(tjs_uint key, tjs_uint32 shift)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyKeyUp(key, shift);
}
void DrawDeviceD3D::OnKeyPress(tjs_uint16 key)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyKeyPress(key);
}
void DrawDeviceD3D::OnMouseWheel(tjs_uint32 shift, tjs_int delta, tjs_int x, tjs_int y)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyMouseWheel(shift, delta, x, y);
}
void DrawDeviceD3D::OnTouchDown(tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyTouchDown(x, y, cx, cy, id);
}
void DrawDeviceD3D::OnTouchUp(tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyTouchUp(x, y, cx, cy, id);
}
void DrawDeviceD3D::OnTouchMove(tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyTouchMove(x, y, cx, cy, id);
}
void DrawDeviceD3D::OnTouchScaling(tjs_real startdist, tjs_real curdist, tjs_real cx, tjs_real cy, tjs_int flag)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyTouchScaling(startdist, curdist, cx, cy, flag);
}
void DrawDeviceD3D::OnTouchRotate(tjs_real startangle, tjs_real curangle, tjs_real dist, tjs_real cx, tjs_real cy, tjs_int flag)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyTouchRotate(startangle, curangle, dist, cx, cy, flag);
}
void DrawDeviceD3D::OnMultiTouch()
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->NotifyMultiTouch();
}
void DrawDeviceD3D::RecheckInputState()
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->RecheckInputState();
}
void DrawDeviceD3D::SetDefaultMouseCursor(iTVPLayerManager* manager)
{
    if (Window)
        Window->SetDefaultMouseCursor();
}
void DrawDeviceD3D::SetMouseCursor(iTVPLayerManager* manager, tjs_int cursor)
{
    if (Window)
        Window->SetMouseCursor(cursor);
}
void DrawDeviceD3D::GetCursorPos(iTVPLayerManager* manager, tjs_int& x, tjs_int& y)
{
    if (Window)
        Window->GetCursorPos(x, y);
}
void DrawDeviceD3D::SetCursorPos(iTVPLayerManager* manager, tjs_int x, tjs_int y)
{
    if (Window)
        Window->SetCursorPos(x, y);
}
void DrawDeviceD3D::SetHintText(iTVPLayerManager* manager, iTJSDispatch2* sender, const ttstr& text)
{
    if (Window)
        Window->SetHintText(sender, text);
}
void DrawDeviceD3D::WindowReleaseCapture(iTVPLayerManager* manager)
{
    if (Window)
        Window->WindowReleaseCapture();
}
void DrawDeviceD3D::SetAttentionPoint(iTVPLayerManager* manager, tTJSNI_BaseLayer* layer, tjs_int l, tjs_int t)
{
    if (Window)
        Window->SetAttentionPoint(layer, l, t);
}
void DrawDeviceD3D::DisableAttentionPoint(iTVPLayerManager* manager)
{
    if (Window)
        Window->DisableAttentionPoint();
}
void DrawDeviceD3D::SetImeMode(iTVPLayerManager* manager, tTVPImeMode mode)
{
    if (Window)
        Window->SetImeMode(mode);
}
void DrawDeviceD3D::ResetImeMode(iTVPLayerManager* manager)
{
    if (Window)
        Window->ResetImeMode();
}
tTJSNI_BaseLayer* DrawDeviceD3D::GetPrimaryLayer()
{
    if (LayerManagerIndex < Managers.size())
        return Managers[LayerManagerIndex].Primary;
    return nullptr;
}
tTJSNI_BaseLayer* DrawDeviceD3D::GetFocusedLayer()
{
    if (LayerManagerIndex < Managers.size())
        return Managers[LayerManagerIndex].Manager->GetFocusedLayer();
    return nullptr;
}
void DrawDeviceD3D::SetFocusedLayer(tTJSNI_BaseLayer* layer)
{
    if (LayerManagerIndex < Managers.size())
        Managers[LayerManagerIndex].Manager->SetFocusedLayer(layer);
}
void DrawDeviceD3D::DumpLayerStructure()
{
    for (auto& mi : Managers)
        if (mi.Manager)
            mi.Manager->DumpLayerStructure();
}

// ===========================================================================
// 合成与呈现
// ===========================================================================
void DrawDeviceD3D::DrawQuadTo(void* target,
                               void* srcTexture,
                               float x,
                               float y,
                               float w,
                               float h,
                               int blendMode,
                               float opacity,
                               const float matrix[16],
                               float u1,
                               float v1)
{
    if (!Backend || !target || !srcTexture)
        return;
    float l = x, t = y, r = x + w, b = y + h;
    float cx0 = l, cy0 = t, cx1 = r, cy1 = t, cx2 = r, cy2 = b, cx3 = l, cy3 = b;
    if (matrix)
    {
        // 2D 仿射：x' = m0*x + m4*y + m12; y' = m1*x + m5*y + m13
        cx0 = matrix[0] * l + matrix[4] * t + matrix[12];
        cy0 = matrix[1] * l + matrix[5] * t + matrix[13];
        cx1 = matrix[0] * r + matrix[4] * t + matrix[12];
        cy1 = matrix[1] * r + matrix[5] * t + matrix[13];
        cx2 = matrix[0] * r + matrix[4] * b + matrix[12];
        cy2 = matrix[1] * r + matrix[5] * b + matrix[13];
        cx3 = matrix[0] * l + matrix[4] * b + matrix[12];
        cy3 = matrix[1] * l + matrix[5] * b + matrix[13];
    }
    // 混合模式 → Layer 合成方法（软件 RenderManager 语义）：
    //   2（覆盖）→ LBM_COPY；0（alpha）→ LBM_ALPHA；1（乘）→ LBM_MUL；
    //   3（加）→ LBM_ADD
    int lbm = krkrsdl3::iTVPRenderBackend::LBM_ALPHA;
    switch (blendMode)
    {
        case 2:
            lbm = krkrsdl3::iTVPRenderBackend::LBM_COPY;
            break;
        case 1:
            lbm = krkrsdl3::iTVPRenderBackend::LBM_MUL;
            break;
        case 3:
            lbm = krkrsdl3::iTVPRenderBackend::LBM_ADD;
            break;
        default:
            lbm = krkrsdl3::iTVPRenderBackend::LBM_ALPHA;
            break;
    }
    Backend->SetTarget(target);
    Backend->ClearTarget(false);
    Backend->LayerSetBlend(lbm, opacity, nullptr);
    if (matrix)
    {
        // 仿射四边形：走网格路径（Layer 合成仅支持矩形）
        float tw = (float)Width, th = (float)Height;
        float v[16] = {
            cx0 / tw * 2.f, cy0 / th * 2.f, 0.f, 0.f, //
            cx1 / tw * 2.f, cy1 / th * 2.f, u1, 0.f, //
            cx2 / tw * 2.f, cy2 / th * 2.f, u1, v1, //
            cx3 / tw * 2.f, cy3 / th * 2.f, 0.f, v1, //
        };
        uint16_t idx[] = {0, 1, 2, 2, 3, 0};
        Backend->SetBlendMode(LayerToMeshBlendMode(lbm), nullptr);
        Backend->DrawMesh(v, 4, idx, 6, srcTexture, opacity);
    }
    else
    {
        Backend->LayerDrawRect(srcTexture, l, t, r - l, b - t, 0.f, 0.f, u1, v1);
    }
}

tTVPBaseTexture* DrawDeviceD3D::RefreshDrawBuffer(size_t index)
{
    // Brings the manager's DrawBuffer (FrontBuff) up to date:
    //  1) SetHoldAlpha(false): ltAlpha layers composite with bmAlphaOnAlpha, so
    //     the buffer keeps real alpha and its transparent areas show the D3D
    //     world layers below;
    //  2) that also means drawing over old pixels would blend with them, so
    //     each rectangle about to be redrawn is cleared to 0,0,0,0 first;
    //  3) only the manager's update region is redrawn. A buffer we did not
    //     compose (new, replaced or resized; the manager fills those with
    //     opaque black) is cleared and redrawn whole.
    if (index >= Managers.size())
        return nullptr;
    tTVPLayerManager* lm = dynamic_cast<tTVPLayerManager*>(Managers[index].Manager);
    if (!lm)
        return nullptr;
    lm->SetHoldAlpha(false);
    for (int pass = 0; pass < 2; ++pass)
    {
        tTVPBaseTexture* buf = lm->GetOrCreateDrawBuffer();
        if (!buf)
            return nullptr;
        auto& info = Managers[index];
        const tjs_int pw = buf->GetWidth(), ph = buf->GetHeight();
        tjs_int lw = 0, lh = 0;
        lm->GetPrimaryLayerSize(lw, lh);
        // A primary larger than the buffer makes the manager grow (and black
        // fill) it mid-composition; redraw everything after that.
        const bool full = buf != info.ComposedBuffer || pw != info.ComposedW || ph != info.ComposedH ||
                          lw > pw || lh > ph;
        tTVPComplexRect& region = lm->GetUpdateRegionForCompletion();
        if (!full && !region.GetCount())
            return buf;
        {
            // This invalidation is consumed by the composition immediately below.
            // Do not turn it into another window update while delivering this one.
            struct RestoreFlag
            {
                bool& flag;
                bool previous;
                ~RestoreFlag() { flag = previous; }
            } restore{RequestingComposition, RequestingComposition};
            RequestingComposition = true;
            if (full)
            {
                buf->Fill(tTVPRect(0, 0, pw, ph), 0x00000000);
                lm->RequestInvalidation(tTVPRect(0, 0, pw, ph));
            }
            else
            {
                // Completion merges the region before drawing it. Merge it here
                // first (merging is idempotent) so exactly the drawn rectangles
                // are cleared.
                lm->NotifyUpdateRegionFixed();
                tTVPRect offset(0, 0, 0, 0);
                if (tTJSNI_BaseLayer* primary = lm->GetPrimaryLayer())
                    offset = primary->GetRect();
                tTVPComplexRect::tIterator it = region.GetIterator();
                while (it.Step())
                {
                    tTVPRect r(*it);
                    r.add_offsets(offset.left, offset.top);
                    tTVPRect clipped;
                    if (TVPIntersectRect(&clipped, r, tTVPRect(0, 0, pw, ph)))
                        buf->Fill(clipped, 0x00000000);
                }
            }
        }
        lm->UpdateToDrawDevice();
        tTVPBaseTexture* composed = lm->GetOrCreateDrawBuffer();
        if (!composed)
            return nullptr;
        // Script run during composition may have changed the manager list.
        if (index >= Managers.size() || Managers[index].Manager != lm)
            return composed;
        auto& after = Managers[index];
        after.ComposedBuffer = composed;
        after.ComposedW = composed->GetWidth();
        after.ComposedH = composed->GetHeight();
        if (composed == buf && after.ComposedW == pw && after.ComposedH == ph)
            return composed;
        // The manager replaced or grew the buffer while drawing.
        after.ComposedBuffer = nullptr;
    }
    return lm->GetOrCreateDrawBuffer();
}

void DrawDeviceD3D::ComposeLayerManager(int index, void* target)
{
    if (index < 0 || index >= (tjs_int)Managers.size() || !Managers[index].Manager || !Backend)
        return;
    tjs_int lw = 0, lh = 0;
    Managers[index].Manager->GetPrimaryLayerSize(lw, lh);
    tTVPBaseTexture* buf = RefreshDrawBuffer(index);
    iTVPTexture2D* tex = buf ? buf->GetTexture() : nullptr;
    if (!tex)
        return;
    const int bufW = tex->GetWidth(), bufH = tex->GetHeight();
    if (bufW <= 0 || bufH <= 0)
        return;
    // Sample only the primary's part of a buffer that may be larger.
    lw = lw > 0 ? std::min<tjs_int>(lw, bufW) : bufW;
    lh = lh > 0 ? std::min<tjs_int>(lh, bufH) : bufH;
    void* srcTex = nullptr;
    // A GPU-resident buffer (the Metal Layer manager) is drawn directly. Its
    // handle uploads pending CPU writes, if any.
    if (!tex->IsCPUResident())
        srcTex = tex->GetTextureHandle();
    if (!srcTex)
    {
        // Software buffer: copy its pixels into a backend texture of its size.
        if (!ScratchTexture || ScratchW != bufW || ScratchH != bufH)
        {
            if (ScratchTexture)
                Backend->DestroyTexture(ScratchTexture);
            ScratchTexture = Backend->CreateTexture(bufW, bufH);
            ScratchW = ScratchTexture ? bufW : 0;
            ScratchH = ScratchTexture ? bufH : 0;
        }
        if (!ScratchTexture)
            return;
        // A scoped lock, never GetTextureData: that pins the texture to the CPU.
        const void* pixels = tex->LockCPURead();
        if (pixels)
            Backend->UpdateTexture(ScratchTexture, (const uint8_t*)pixels, bufW, bufH, tex->GetPitch());
        tex->UnlockCPU();
        if (!pixels)
            return;
        srcTex = ScratchTexture;
    }
    // mode 0 = LBM_ALPHA：透明区域露出下层（D3D 世界层等）
    DrawQuadTo(target, srcTex, 0, 0, (float)Width, (float)Height, 0, 1.0f, nullptr,
               (float)lw / bufW, (float)lh / bufH);
}

// 绘制单个 D3D 层（Back/Front 平面共用）。
// D3D 层坐标以画面中心为原点（与网格 NDC 一致）。平移且落在整数像素上的
// 图片走后端的普通 Layer 合成（与 Layer 完全相同的混合公式）；其余（仿射、
// 亚像素位置、无该能力的后端）走网格路径，混合由 MBM_LAYER_* 模式给出。
static void DrawD3DLayerPictures(DrawDeviceD3D* dev, void* target, D3DLayer* layer,
                                 float offsetX, float offsetY)
{
    krkrsdl3::iTVPRenderBackend* backend = dev->GetBackend();
    const float tw = (float)dev->GetWidth(), th = (float)dev->GetHeight();
    const float* m = layer->Matrix;
    const bool translateOnly = m[0] == 1.0f && m[1] == 0.0f && m[4] == 0.0f && m[5] == 1.0f;
    for (auto* pic : layer->Pictures)
    {
        if (!pic || !pic->Image || !pic->Image->Texture || pic->opacity <= 0)
            continue;
        const PictureBlend blend = PictureBlendFor(pic->blendMode, pic->opacity);
        const int opacity = std::min<tjs_int>(pic->opacity, 255);
        float opa = (float)opacity / 255.0f;
        float px = (float)pic->DstX + pic->CoordX + offsetX;
        float py = (float)pic->DstY + pic->CoordY + offsetY;
        float pw = (float)pic->SrcW, ph = (float)pic->SrcH;
        float sw = (float)pic->Image->Width, sh = (float)pic->Image->Height;
        if (pw <= 0 || ph <= 0 || sw <= 0 || sh <= 0)
            continue;
        float u0 = (float)pic->SrcX / sw, v0 = (float)pic->SrcY / sh;
        float u1 = (float)(pic->SrcX + pic->SrcW) / sw, v1 = (float)(pic->SrcY + pic->SrcH) / sh;

        backend->SetTarget(target);
        backend->ClearTarget(false);

        if (translateOnly)
        {
            // Screen pixel of the picture's top-left corner.
            const float x = px + m[12] + tw * 0.5f, y = py + m[13] + th * 0.5f;
            if (x == std::floor(x) && y == std::floor(y) && std::abs(x) < 1e6f && std::abs(y) < 1e6f)
            {
                if (OperatePictureRect(backend, target, (int)tw, (int)th, pic, (int)x, (int)y))
                    continue;
                backend->LayerSetBlend(blend.lbm, opa, nullptr);
                backend->LayerDrawRect(pic->Image->Texture, x, y, pw, ph, u0, v0, u1, v1);
                continue;
            }
        }

        // 仿射：4 角经矩阵变换 → NDC（内容坐标 y 向下，顶=NDC -1，与 Layer 路径一致）
        float l = px, t = py, r = px + pw, b = py + ph;
        float cx0 = m[0] * l + m[4] * t + m[12];
        float cy0 = m[1] * l + m[5] * t + m[13];
        float cx1 = m[0] * r + m[4] * t + m[12];
        float cy1 = m[1] * r + m[5] * t + m[13];
        float cx2 = m[0] * r + m[4] * b + m[12];
        float cy2 = m[1] * r + m[5] * b + m[13];
        float cx3 = m[0] * l + m[4] * b + m[12];
        float cy3 = m[1] * l + m[5] * b + m[13];
        float v[16] = {
            cx0 / tw * 2.f, cy0 / th * 2.f, u0, v0, //
            cx1 / tw * 2.f, cy1 / th * 2.f, u1, v0, //
            cx2 / tw * 2.f, cy2 / th * 2.f, u1, v1, //
            cx3 / tw * 2.f, cy3 / th * 2.f, u0, v1, //
        };
        uint16_t idx[] = {0, 1, 2, 2, 3, 0};
        backend->SetBlendMode(blend.mesh, nullptr);
        backend->DrawMesh(v, 4, idx, 6, pic->Image->Texture, opa);
    }
}

void DrawDeviceD3D::RenderFrame()
{
    EnsureBackend();
    if (!Backend)
        return;

    // 合成目标（虚拟屏幕尺寸）
    if (!CompositeTarget)
        CompositeTarget = Backend->CreateTarget(Width, Height);
    if (!CompositeTarget)
    {
        TVPConsoleLog("D3D RenderFrame: no composite target");
        return;
    }


    // 清屏
    Backend->SetTarget(CompositeTarget);
    Backend->ClearTarget(true);

    // 合成顺序（对应 KAG 非 D3D 的表裏双页机制，见 docs/drawdevice-d3d.md）：
    //   - Back 平面 D3D 层 = "裏"画面（world.basePlane==1，转场期间世界层所在）。
    //     非 D3D 语义：裏（back.base）平时隐藏（initBaseVisible 置
    //     back.base.visible=false），仅在转场期间作为转场源参与合成；
    //     因此这里只在本设备转场进行中绘制 Back 平面层。
    //   - LayerManager（DrawBuffer） = 表画面的普通 Layer 树（软渲染合成），
    //     画在世界层之上（消息/UI 高于世界层，对应非 D3D 的 absolute 层序）；
    //     DrawBuffer 透明区域露出世界层。
    //   - Front/Both 平面 D3D 层 = "表"画面（world.basePlane==0，平时世界层所在）。
    std::vector<D3DLayer*> backLayers, frontLayers;
    for (auto* l : D3DLayers)
    {
        if (l->drawPlane == D3DLayer::DrawPlaneBack)
            backLayers.push_back(l);
        else
            frontLayers.push_back(l);
    }
    auto sortByIndex = [](std::vector<D3DLayer*>& v) {
        std::stable_sort(v.begin(), v.end(),
                         [](D3DLayer* a, D3DLayer* b) { return a->frontIndex < b->frontIndex; });
    };
    sortByIndex(backLayers);
    sortByIndex(frontLayers);

    // 只合成主 layer manager（layerManagerIndex，脚本可设；默认 0）。
    // 其它 manager 属于子窗口/辅助窗口，非 D3D 模式下不参与主窗口显示，
    // 若一并合成会以空白白底覆盖主画面。
   
    // 表/裏页面由 LayerDrawIndex 在 1 <-> 2 之间翻转：
    //   - 平时绘制 LayerDrawIndex 指向的当前页面；
    //   - startTransition() 已把开始转场前的完整当前画面保存到 PrevCompositeTarget；
    //   - 转场进行中，当前帧必须先绘制“下一页”（LayerDrawIndex 的另一侧），
    //     再把 PrevCompositeTarget 以 1-progress 叠上去。
    // 如果转场中仍绘制 LayerDrawIndex 本身，画面会一直停在旧页，直到 stopTransition()
    // 翻转索引后才瞬间变成新页，看不到过渡。
    tjs_int activeLayerDrawIndex = LayerDrawIndex;
    if (TransitionActive)
        activeLayerDrawIndex = (LayerDrawIndex == 1 ? 2 : 1);

    // 先画 D3D 世界层，再在其上合成 LayerManager DrawBuffer（消息/UI）。
    // DrawBuffer 透明区域露出世界层。Back 平面不是“旧画面源”——旧画面已经在
    // PrevCompositeTarget 里；转场期间也应按 activeLayerDrawIndex 绘制下一页。
    for (auto* l : frontLayers)
    {
        DrawD3DLayerPictures(this, CompositeTarget, l, (float)OffsetX, (float)OffsetY);
        l->DrawEmoteTarget(CompositeTarget);
    }
    if (activeLayerDrawIndex == 2)
    {
        for (auto* l : backLayers)
        {
            DrawD3DLayerPictures(this, CompositeTarget, l, (float)OffsetX, (float)OffsetY);
            l->DrawEmoteTarget(CompositeTarget);
        }
    }
    // EmotePlayer 属于其所属的 D3DLayer，由 D3DLayer 在对应平面绘制。
    if (activeLayerDrawIndex >= 1 && activeLayerDrawIndex < (tjs_int)Managers.size())
    {
        ComposeLayerManager(activeLayerDrawIndex, CompositeTarget);
    }
    if (LayerManagerIndex == 3 && LayerManagerIndex < Managers.size())
    {
        ComposeLayerManager(LayerManagerIndex, CompositeTarget);
    }

    // 转场交叉淡化：结果 = prev*(1-p) + next*p。
    // 这里 next 已经完整绘入 CompositeTarget；再叠加 prev*(1-p)，视觉上等价于
    // 从开始快照逐渐露出下一页。
    if (TransitionActive && PrevCompositeTarget && TransitionProgress < 1.0f)
    {
        void* prevTex = Backend->GetTargetTexture(PrevCompositeTarget);
        if (prevTex)
        {
            float opa = 1.0f - TransitionProgress;
            float v[16] = {
                -1.f, -1.f, 0.f, 0.f, //
                1.f,  -1.f, 1.f, 0.f, //
                1.f,  1.f, 1.f, 1.f, //
                -1.f, 1.f, 0.f, 1.f, //
            };
            uint16_t idx[] = {0, 1, 2, 2, 3, 0};
            Backend->SetTarget(CompositeTarget);
            Backend->ClearTarget(false);
            Backend->SetBlendMode(0, nullptr);
            Backend->DrawMesh(v, 4, idx, 6, prevTex, opa);
        }
    }
    if (TransitionActive && TransitionProgress >= 1.0f)
        stopTransition();
}

void DrawDeviceD3D::PresentToWindow()
{
    // 统一呈现路径：合成目标（compositor 体系的后端离屏目标）→ TVPWindow::PresentTexture
    //   - GPU 后端：目标纹理直接呈现（sprite 别名，零拷贝；TVPRenderOnce 绘制之）
    //   - 软件后端：目标（CPU 缓冲）→ 一般贴图中转 → 窗口上传（SW 保底路径）
    // 窗口可见性/尺寸由 TVPWindow 统一管理，此处不再直接操作 sprite。
    EnsureBackend();
    if (!Backend || !Window || !CompositeTarget)
        return;

    if (IsSoftwareBackend())
    {
        if (!PresentScratchTexture)
            PresentScratchTexture = Backend->CreateTexture(Width, Height);
        if (!PresentScratchTexture)
            return;
        int pitch = 0;
        uint8_t* pixels = Backend->LockTarget(CompositeTarget, pitch);
        if (pixels)
        {
            Backend->UpdateTexture(PresentScratchTexture, pixels, Width, Height, pitch);
            Backend->UnlockTarget(CompositeTarget);
        }
        Window->PresentTexture(PresentScratchTexture, Width, Height);
        return;
    }

    Window->PresentTexture(Backend->GetTargetTexture(CompositeTarget), Width, Height);
}

void DrawDeviceD3D::Update()
{
    RenderFrame();
}

void DrawDeviceD3D::Show()
{
    PresentToWindow();
}

// ===========================================================================
// 脚本 API
// ===========================================================================
void DrawDeviceD3D::update(tjs_real diffTime)
{
    // 转场进度由脚本通过 transState 驱动
    if (TransitionActive)
    {
        TransitionProgress = 1.0 - TransState;
        if (TransitionProgress < 0)
            TransitionProgress = 0;
        if (TransitionProgress > 1)
            TransitionProgress = 1;
    }
    // 驱动各 D3DLayer 的脚本 onUpdate(diff)：
    // D3DAffineLayer 等脚本子类在此计算仿射矩阵（setMatrix）并同步 picture 属性，
    // 与普通 Layer 的 onPaint 链对应（KAG 每帧调用 drawDevice.update(diffTime)）。
    for (auto* l : D3DLayers)
    {
        if (l)
            l->DriveOnUpdate(diffTime);
    }
    // Script updates and Layer notifications share the window's update queue.
    // The dispatcher coalesces them into one composition/presentation, instead
    // of rendering here and again for each pending window invalidation.
    if (Window)
        Window->RequestUpdate();
    else
        RenderFrame();
}

void DrawDeviceD3D::capture(iTJSDispatch2* layerObj, tjs_int index)
{
    tTJSNI_BaseLayer* layer = nullptr;
    if (layerObj && layerObj->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                                    (iTJSNativeInstance**)&layer) < 0)
        return;
    if (!layer)
        return;

    // index 映射到层管理器：0=首个；>=5000000 时为绝对序号（indexBase + 序号）
    size_t mi = 0;
    if (index >= 5000000)
        mi = (size_t)(index - 5000000);
    else
        mi = (size_t)index;
    if (mi >= Managers.size() || !Managers[mi].Manager)
        return;
    // The same partial redraw as presentation, so neither leaves the other a
    // consumed update region over uncleared pixels.
    iTVPBaseBitmap* buf = RefreshDrawBuffer(mi);
    if (!buf)
        return;
    iTVPTexture2D* tex = buf->GetTexture();
    if (!tex)
        return;

    // 拷贝到目标 Layer 的 MainImage（GPU 拷贝；不共享）
    iTVPRenderManager* rm = TVPGetRenderManager();
    iTVPTexture2D* copy = rm->CreateTexture2D(tex->GetWidth(), tex->GetHeight(), tex);
    if (copy)
        layer->AssignTexture(copy);
}

void DrawDeviceD3D::startTransition(iTJSDispatch2* dict)
{
    if (!dict || !Backend || !CompositeTarget)
        return;
    // 保存当前画面快照
    if (PrevCompositeTarget)
    {
        Backend->DestroyTarget(PrevCompositeTarget);
        PrevCompositeTarget = nullptr;
    }
    PrevCompositeTarget = Backend->CreateTarget(Width, Height);
    if (!PrevCompositeTarget)
        return;
    void* curTex = Backend->GetTargetTexture(CompositeTarget);
    if (curTex)
        DrawQuadTo(PrevCompositeTarget, curTex, 0, 0, (float)Width, (float)Height, 2, 1.0f);
    TransitionActive = true;
    TransitionProgress = 0.0f;
    TransState = 1.0f;
}

void DrawDeviceD3D::stopTransition()
{
    TransitionActive = false;
    TransitionProgress = 1.0f;
    TransState = 1.0f;
    if (Backend && PrevCompositeTarget)
    {
        Backend->DestroyTarget(PrevCompositeTarget);
        PrevCompositeTarget = nullptr;
        LayerDrawIndex = (LayerDrawIndex == 1 ? 2 : 1);
    }
}

void DrawDeviceD3D::setScreenRect(tjs_int x, tjs_int y, tjs_int w, tjs_int h)
{
    ScreenX = x;
    ScreenY = y;
    ScreenW = w;
    ScreenH = h;
}

void DrawDeviceD3D::setPrimarySize(tjs_int w, tjs_int h)
{
    Width = w;
    Height = h;
    if (Backend)
    {
        if (CompositeTarget)
        {
            Backend->DestroyTarget(CompositeTarget);
            CompositeTarget = nullptr;
        }
        stopTransition();
        CompositeTarget = Backend->CreateTarget(Width, Height);
    }
}

void DrawDeviceD3D::setOffset(tjs_int x, tjs_int y)
{
    OffsetX = x;
    OffsetY = y;
}

void DrawDeviceD3D::recreate()
{
    // 设备重建（窗口重建时）：先经旧后端释放本设备的全部句柄，再重取后端。
    // A transition snapshot does not survive this, so the transition ends.
    stopTransition();
    if (Window)
        Window->ReleaseBorrowedTexture();
    ReleaseBackendResources();
    Backend = krkrsdl3::TVPGetRenderBackend();
}

void DrawDeviceD3D::finalize()
{
    //delete this;
}

tTJSVariant DrawDeviceD3D::getModule(tTJSString name)
{
    if (name == TJS_N("emote"))
    {
        iTJSDispatch2* dict = TJSCreateDictionaryObject();
        tTJSVariant maskMode(MaskMode);
        static tjs_uint hint = 0;
        dict->PropSet(TJS_MEMBERENSURE, TJS_N("maskMode"), &hint, &maskMode, dict);
        return tTJSVariant(dict, dict);
    }
    return tTJSVariant();
}

tTJSVariant DrawDeviceD3D::getPrimaryLayers()
{
    iTJSDispatch2* arr = TJSCreateArrayObject();
    tjs_int idx = 0;
    for (auto& mi : Managers)
    {
        if (mi.Primary)
        {
            iTJSDispatch2* owner = mi.Primary->GetOwnerNoAddRef();
            if (owner)
            {
                tTJSVariant v(owner, owner);
                arr->PropSetByNum(TJS_MEMBERENSURE, idx++, &v, arr);
            }
        }
    }
    return tTJSVariant(arr, arr);
}

tTJSVariant DrawDeviceD3D::getChildren()
{
    // children 属性：返回设备上全部 D3DLayer 的脚本对象数组。
    // 脚本侧用它（drawDevice.children*）+ 元素的 absolute 属性做命中判定排序，
    // 元素是带 checkTouch 的脚本层。此前无此属性，脚本侧取不到 D3D 层，
    // D3D 层上的触摸判定整体失效
    iTJSDispatch2* arr = TJSCreateArrayObject();
    tjs_int idx = 0;
    for (auto* layer : D3DLayers)
    {
        if (layer && layer->ScriptObject)
        {
            tTJSVariant v(layer->ScriptObject, layer->ScriptObject);
            arr->PropSetByNum(TJS_MEMBERENSURE, idx++, &v, arr);
        }
    }
    return tTJSVariant(arr, arr);
}

// ===========================================================================
// D3DLayer
// ===========================================================================
D3DLayer::D3DLayer(iTJSDispatch2* d3dDeviceObj, iTJSDispatch2* scriptObject)
{
    DrawDeviceD3D* dev = GetDrawDeviceInstance(d3dDeviceObj);
    if (dev)
    {
        Device = dev;
        Device->RegisterD3DLayer(this);
    }
    if (scriptObject)
    {
        ScriptObject = scriptObject;
        ScriptObject->AddRef();
    }
}

void D3DLayer::DriveOnUpdate(tjs_real diffTime)
{
    // D3DAffineLayer 等脚本子类在 onUpdate(diff) 里计算并 setMatrix / 更新 picture
    // 属性（见 out/data/system/D3D.tjs）。原生 D3DLayer 无此方法时静默跳过。
    if (!ScriptObject)
        return;
    try
    {
        tTJSVariant vDiff(diffTime);
        tTJSVariant* p = &vDiff;
        ScriptObject->FuncCall(0, TJS_N("onUpdate"), nullptr, nullptr, 1, &p, ScriptObject);
    }
    catch (...)
    {
        // onUpdate 不存在或抛异常：非致命
    }
}

D3DLayer::~D3DLayer()
{
    if (ScriptObject)
    {
        ScriptObject->Release();
        ScriptObject = nullptr;
    }
    if (Device)
        Device->UnregisterD3DLayer(this);
    // 图片对象由脚本（ncb native instance）持有，这里只解除回指并清空列表，
    // 否则 D3DPicture 析构时会访问已销毁的 Layer（悬垂崩溃）。
    for (auto* p : Pictures)
    {
        if (p)
            p->Layer = nullptr;
    }
    Pictures.clear();
    if (Device && Device->GetBackend())
    {
        if (EmoteTarget)
            Device->GetBackend()->DestroyTarget(EmoteTarget);
        if (EmoteMaskTarget)
            Device->GetBackend()->DestroyTarget(EmoteMaskTarget);
    }
    EmoteTarget = nullptr;
    EmoteMaskTarget = nullptr;
}

void D3DLayer::setMatrix(tjs_real m11, tjs_real m12, tjs_real m13, tjs_real m14, tjs_real m21,
                         tjs_real m22, tjs_real m23, tjs_real m24, tjs_real m31, tjs_real m32,
                         tjs_real m33, tjs_real m34, tjs_real m41, tjs_real m42, tjs_real m43,
                         tjs_real m44)
{
    // 行优先 16 值；2D 仿射用 m0/m1/m4/m5/m12/m13
    Matrix[0] = (float)m11;
    Matrix[1] = (float)m12;
    Matrix[2] = (float)m13;
    Matrix[3] = (float)m14;
    Matrix[4] = (float)m21;
    Matrix[5] = (float)m22;
    Matrix[6] = (float)m23;
    Matrix[7] = (float)m24;
    Matrix[8] = (float)m31;
    Matrix[9] = (float)m32;
    Matrix[10] = (float)m33;
    Matrix[11] = (float)m34;
    Matrix[12] = (float)m41;
    Matrix[13] = (float)m42;
    Matrix[14] = (float)m43;
    Matrix[15] = (float)m44;
}

void D3DLayer::AddPicture(class D3DPicture* pic)
{
    Pictures.push_back(pic);
}

void D3DLayer::RemovePicture(class D3DPicture* pic)
{
    for (auto it = Pictures.begin(); it != Pictures.end(); ++it)
    {
        if (*it == pic)
        {
            Pictures.erase(it);
            break;
        }
    }
}

void D3DLayer::DrawEmoteTarget(void* target)
{
    if (!EmoteTarget || !Device || !Device->GetBackend())
        return;
    void* tex = Device->GetBackend()->GetTargetTexture(EmoteTarget);
    if (!tex)
        return;
    Device->GetBackend()->SetTarget(target);
    Device->GetBackend()->LayerSetBlend(krkrsdl3::iTVPRenderBackend::LBM_ALPHA, 1.0f, nullptr);
    Device->GetBackend()->LayerDrawRect(tex, 0, 0, (float)Device->GetWidth(), (float)Device->GetHeight());
}

void D3DLayer::finalize()
{
    // 此函数只用于数据清理，不用于实例删除，否则析构函数会二次删除
    //delete this;
}

// ===========================================================================
// D3DImage
// ===========================================================================
D3DImage::D3DImage(iTJSDispatch2* drawDeviceObj)
{
    DrawDeviceD3D* dev = GetDrawDeviceInstance(drawDeviceObj);
    if (dev)
        Device = dev;
}

D3DImage::~D3DImage()
{
    ReleaseTexture();
}

void D3DImage::ReleaseTexture()
{
    if (Device && Device->GetBackend())
    {
        if (Target)
            Device->GetBackend()->DestroyTarget(Target);
        else if (Texture)
            Device->GetBackend()->DestroyTexture(Texture);
    }
    Texture = nullptr;
    Target = nullptr;
}

void D3DImage::load(iTJSDispatch2* layerObj)
{
    tTJSNI_BaseLayer* layer = nullptr;
    if (layerObj && layerObj->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                                    (iTJSNativeInstance**)&layer) < 0)
        return;
    if (!layer || !Device || !Device->GetBackend())
        return;
    tTVPBaseTexture* main = layer->GetMainImage();
    if (!main)
        return;
    tjs_int w = main->GetWidth(), h = main->GetHeight();
    if (w <= 0 || h <= 0)
        return;

    krkrsdl3::iTVPRenderBackend* backend = Device->GetBackend();
    ReleaseTexture();
    Width = w;
    Height = h;

    iTVPTexture2D* tex = main->GetTexture();
    if (!tex)
    {
        Texture = backend->CreateTexture(w, h);
        return;
    }
    // A GPU-resident Layer image (Metal Layer manager) is copied on the GPU.
    // Reading it back would stall, and GetTextureData would also pin it to the
    // CPU, sending that Layer's own drawing to software for a while.
    if (!tex->IsCPUResident() && tex->GetFormat() == TVPTextureFormat::RGBA)
    {
        void* source = tex->GetTextureHandle();
        void* copy = source ? backend->CreateTarget(w, h) : nullptr;
        void* sampled = copy ? backend->GetTargetTexture(copy) : nullptr;
        if (sampled && backend->CopyTargetToLayerTexture(source, copy))
        {
            Target = copy;
            Texture = sampled;
            return;
        }
        if (copy)
            backend->DestroyTarget(copy);
    }
    // 从 Layer 主图像拷贝像素（CPU 读取后上传；scoped lock, so nothing stays pinned）
    Texture = backend->CreateTexture(w, h);
    if (!Texture)
        return;
    const void* pixels = tex->LockCPURead();
    if (pixels)
        backend->UpdateTexture(Texture, (const uint8_t*)pixels, w, h, tex->GetPitch());
    tex->UnlockCPU();
}

void D3DImage::finalize()
{
    // 此函数只用于数据清理，不用于实例删除，否则析构函数会二次删除
    //delete this;
}

// ===========================================================================
// D3DPicture
// ===========================================================================
D3DPicture::D3DPicture(iTJSDispatch2* layerObj, iTJSDispatch2* imageObj)
{
    Layer = ncbInstanceAdaptor<D3DLayer>::GetNativeInstance(layerObj);
    if (Layer)
        Layer->AddPicture(this);
    Image = ncbInstanceAdaptor<D3DImage>::GetNativeInstance(imageObj); // 引用图片（不拥有）
}

D3DPicture::~D3DPicture()
{
    if (Layer)
        Layer->RemovePicture(this);
}

void D3DPicture::assignImageRange(tjs_int sx, tjs_int sy, tjs_int sw, tjs_int sh, tjs_int dx,
                                  tjs_int dy)
{
    SrcX = sx;
    SrcY = sy;
    SrcW = sw;
    SrcH = sh;
    DstX = dx;
    DstY = dy;
}

void D3DPicture::finalize()
{
    // 此函数只用于数据清理，不用于实例删除，否则析构函数会二次删除
    //delete this;
}

// ===========================================================================
// DrawDeviceD3D 内部
// ===========================================================================
void DrawDeviceD3D::UnregisterD3DLayer(class D3DLayer* layer)
{
    for (auto it = D3DLayers.begin(); it != D3DLayers.end(); ++it)
    {
        if (*it == layer)
        {
            D3DLayers.erase(it);
            break;
        }
    }
}

} // namespace drawdevice
