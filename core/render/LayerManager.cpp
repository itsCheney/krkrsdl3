//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Layer Management
//---------------------------------------------------------------------------

#include "tjsCommHead.h"

#include "LayerManager.h"
#include "PointReadTrace.h"
#include "TVPMsg.h"
#include "LayerBitmap.h"
#include "RenderManager.h"
#include "TVPStorage.h"
#include "TVPEvent.h"
#include "TVPSystem.h"
#include "TVPDebug.h"
#include "LayerTreeOwner.h"

#include "tjsNativeLayer.h"
#include "AsyncAlphaTileCache.h"
#include "MetalLayerRenderManager.h"
#include "../../plugins/emoteplayer/emoteperformance.h"
#include <algorithm>
#include <unordered_map>

namespace {
std::vector<tTVPLayerManager*> asyncInputManagers;

bool TVPIsLiveAlphaLayer(const std::vector<tTJSNI_BaseLayer*>& nodes,
                         tTJSNI_BaseLayer* layer, uint64_t lifetimeID) {
    if(!layer) return false;
    const auto it=std::find(nodes.begin(),nodes.end(),layer);
    return it!=nodes.end() && layer->GetLifetimeID()==lifetimeID;
}
}
struct tTVPLayerManager::AlphaPointerFrame {
    struct Point {
        tTJSNI_BaseLayer* layer = nullptr;
        uint64_t lifetimeID = 0;
        int x=0,y=0,width=0,height=0,px=0,py=0;
        uint64_t textureID=0;
        tjs_int hitType=0;
        bool inside=false, async=false, mask=false, sourceBound=false;
        tjs_uint32 alpha=0; // 256 means outside MainImage, independently of threshold.
        std::shared_ptr<krkrsdl3::AsyncAlphaTile> tile;
        std::shared_ptr<krkrsdl3::AsyncAlphaTileSnapshot> snapshot;
        // No CPU full-image copy. Keeping the original GPU texture intrusive
        // reference makes the existing Bitmap COW preserve F across animation.
        std::shared_ptr<iTVPTexture2D> sourceTexture;
    };
    int x=0,y=0;
    bool canceled=false;
    uint64_t pointerChain=0;
    std::shared_ptr<krkrsdl3::AsyncLayerPresentation> presentation;
    std::function<void()> dispatch;
    std::unordered_map<tTJSNI_BaseLayer*,Point> points;
};
tTVPLayerManager::AlphaPointerScope::AlphaPointerScope(tTVPLayerManager& m,
    tjs_int x,tjs_int y,std::function<void()> dispatch,uint64_t pointerID,bool startsChain,bool endsChain) : manager(m) {
    if(m.AlphaDispatching || !emoteplayer::performanceEnabled("MIKAGE_EMOTE_ASYNC_ALPHA")) return;
    if(startsChain || !m.AlphaPointerChains.count(pointerID)) {
        auto prior=m.AlphaPointerChains.find(pointerID);
        if(prior!=m.AlphaPointerChains.end()) m.CanceledAlphaPointerChains.erase(prior->second);
        m.AlphaPointerChains[pointerID]=m.NextAlphaPointerChain++;
    }
    if(m.CanceledAlphaPointerChains.count(m.AlphaPointerChains[pointerID])) {
        if(endsChain) { m.CanceledAlphaPointerChains.erase(m.AlphaPointerChains[pointerID]); m.AlphaPointerChains.erase(pointerID); }
        deferred=true; return;
    }
    auto frame=m.PrepareAlphaPointer(x,y,std::move(dispatch));
    frame->pointerChain=m.AlphaPointerChains[pointerID];
    if(endsChain) m.AlphaPointerChains.erase(pointerID); // Touch IDs do not accumulate across sessions.
    if(frame->canceled) { deferred=true; return; }
    const bool ready=m.ResolveAlphaPointer(frame);
    if(frame->canceled) { deferred=true; return; }
    if(!m.PendingAlphaInput.empty() || !ready) {
        m.PendingAlphaInput.push_back(std::move(frame));
        emoteplayer::performanceCounters().alphaPendingEvents.fetch_add(1,std::memory_order_relaxed);
        deferred=true;
        return;
    }
    m.ActiveAlphaInput=std::move(frame); m.AlphaDispatching=true; entered=true;
}
tTVPLayerManager::AlphaPointerScope::~AlphaPointerScope() {
    if(entered) { manager.ActiveAlphaInput.reset(); manager.AlphaDispatching=false; manager.AlphaQuery=false; }
}
std::shared_ptr<tTVPLayerManager::AlphaPointerFrame> tTVPLayerManager::PrepareAlphaPointer(
    tjs_int x,tjs_int y,std::function<void()> dispatch) {
    auto frame=std::make_shared<AlphaPointerFrame>();
    frame->x=x; frame->y=y; frame->dispatch=std::move(dispatch);
    // Record even non-alpha ancestors: moving a parent while a read is pending
    // must not make the replay chase a new tile indefinitely.
    for(auto* layer:GetAllNodes()) {
        if(!layer || !layer->GetOwnerNoAddRef() || !layer->GetNodeVisible()) continue;
        AlphaPointerFrame::Point point;
        point.layer=layer;
        point.lifetimeID=layer->GetLifetimeID();
        point.x=x; point.y=y; layer->FromPrimaryCoordinates(point.x,point.y);
        point.width=layer->GetRect().get_width(); point.height=layer->GetRect().get_height();
        point.inside=point.x>=0 && point.y>=0 && point.x<point.width && point.y<point.height;
        point.hitType=tjs_int(layer->GetHitType()); point.mask=point.hitType==htMask;
        auto* image=layer->GetMainImage();
        if(point.inside && point.mask && image) {
            auto* texture=image->GetTexture();
            point.px=point.x-layer->GetImageLeft(); point.py=point.y-layer->GetImageTop();
            uint64_t version=0;
            texture->GetContentKey(point.textureID,version);
            if(point.px<0 || point.py<0 || point.px>=int(image->GetWidth()) || point.py>=int(image->GetHeight()))
                point.alpha=256;
            else if(layer->GetHitThreshold()>0) {
                point.async=TVPRequestEmoteAsyncAlpha(texture,point.px,point.py,point.tile);
                if(!point.async) point.alpha=image->GetBPP()==32 ? texture->GetPointAlpha(point.px,point.py) :
                    image->GetPoint(point.px,point.py)>>24;
            }
        }
        frame->points.emplace(layer,std::move(point));
    }
    return frame;
}
void tTVPLayerManager::CancelAlphaPointerChain(uint64_t chain) {
    CanceledAlphaPointerChains.insert(chain);
    for(auto& pending:PendingAlphaInput) if(pending->pointerChain==chain) pending->canceled=true;
}
void tTVPLayerManager::BindAlphaPresentation() {
    const auto ticket=TVPGetEmoteAlphaPresentation();
    if(!ticket) return;
    for(auto& frame:PendingAlphaInput) {
        if(frame->canceled || frame->presentation) continue;
        const auto& nodes=GetAllNodes();
        bool removed=false;
        for(const auto& pair:frame->points)
            if(!TVPIsLiveAlphaLayer(nodes,pair.first,pair.second.lifetimeID)) { removed=true; break; }
        if(removed) { frame->canceled=true; CancelAlphaPointerChain(frame->pointerChain); continue; }
        // Bind only once, before callbacks. Resize/COW while waiting for this
        // first composition use the new epoch and its actual coordinate map.
        auto refreshed=PrepareAlphaPointer(frame->x,frame->y,{});
        frame->points=std::move(refreshed->points);
        frame->presentation=ticket;
    }
}
void tTVPLayerManager::CaptureAlphaForPresentation(tTJSNI_BaseLayer* layer) {
    const auto ticket=TVPGetEmoteAlphaPresentation();
    if(!ticket || !emoteplayer::performanceEnabled("MIKAGE_EMOTE_ASYNC_ALPHA")) return;
    for(auto& frame:PendingAlphaInput) {
        if(frame->canceled || frame->presentation!=ticket) continue;
        auto found=frame->points.find(layer);
        if(found==frame->points.end() || found->second.sourceBound) continue;
        auto& point=found->second;
        const auto& nodes=GetAllNodes();
        if(!TVPIsLiveAlphaLayer(nodes,layer,point.lifetimeID)) {
            frame->canceled=true; CancelAlphaPointerChain(frame->pointerChain); continue;
        }
        point.x=frame->x; point.y=frame->y; layer->FromPrimaryCoordinates(point.x,point.y);
        point.width=layer->GetRect().get_width(); point.height=layer->GetRect().get_height();
        point.inside=layer->GetNodeVisible() && point.x>=0 && point.y>=0 &&
            point.x<point.width && point.y<point.height;
        point.hitType=tjs_int(layer->GetHitType()); point.mask=point.hitType==htMask;
        point.alpha=0; point.async=false; point.tile.reset(); point.snapshot.reset(); point.sourceTexture.reset();
        auto* image=layer->GetMainImage();
        // GetImageLeft/Top throw "Not drawable layer type" without a MainImage;
        // image-less candidates keep alpha 0 and no pixel coordinate.
        point.px=0; point.py=0;
        if(image) { point.px=point.x-layer->GetImageLeft(); point.py=point.y-layer->GetImageTop(); }
        if(point.inside && point.mask && image && (point.px<0 || point.py<0 ||
           point.px>=int(image->GetWidth()) || point.py>=int(image->GetHeight()))) point.alpha=256;
        if(point.inside && point.mask && layer->GetHitThreshold()>0 && image &&
           point.px>=0 && point.py>=0 && point.px<int(image->GetWidth()) && point.py<int(image->GetHeight())) {
            uint64_t version=0; image->GetTexture()->GetContentKey(point.textureID,version);
            point.async=TVPRequestEmoteAsyncAlpha(image->GetTexture(),point.px,point.py,point.tile);
            if(point.async) {
                auto* source=image->GetTexture(); source->AddRef();
                point.sourceTexture=std::shared_ptr<iTVPTexture2D>(source,[](iTVPTexture2D* t){t->Release();});
            }
            if(!point.async) point.alpha=image->GetBPP()==32 ? image->GetTexture()->GetPointAlpha(point.px,point.py) :
                image->GetPoint(point.px,point.py)>>24;
        }
        point.sourceBound=true;
    }
}
void tTVPLayerManager::FinishAlphaPresentation() {
    // First consume all ready requests, including later queued events. FIFO
    // orders callbacks, not storage release; otherwise an earlier missing tile
    // can be blocked forever by later events owning the bounded read budget.
    for(auto& frame:PendingAlphaInput) for(auto& pair:frame->points) {
        auto& point=pair.second;
        if(point.snapshot && point.snapshot->Materialize()) point.sourceTexture.reset();
    }
    for(auto& frame:PendingAlphaInput) {
        const auto& ticket=frame->presentation;
        if(frame->canceled || !ticket || ticket->failed.load(std::memory_order_acquire)) continue;
        for(auto& pair:frame->points) {
            auto& point=pair.second;
            if(!point.async || point.snapshot || !point.sourceBound) continue;
            // Missing requests retry the retained source F on a subsequent
            // normal frame. Never throw away the other candidates' F snapshots.
            if(point.sourceTexture && point.tile) {
                point.tile->demanded=true;
                TVPEncodeFrozenEmoteAsyncAlpha(point.sourceTexture.get(),ticket);
            }
            point.snapshot=point.tile ? point.tile->RequestAtFrame(ticket->frameSerial) : nullptr;
            if(point.snapshot && point.snapshot->Ticket()!=ticket) point.snapshot.reset();
            if(point.snapshot && point.snapshot->Materialize()) point.sourceTexture.reset();
        }
    }
}
bool tTVPLayerManager::ResolveAlphaPointer(const std::shared_ptr<AlphaPointerFrame>& frame) {
    if(frame->canceled) return true;
    // Membership and generation identity precede every raw-pointer getter/update,
    // including retry after a failed drawable. A recycled NI address cannot
    // make an old event target a new native layer instance.
    const auto& nodes=GetAllNodes();
    for(const auto& pair:frame->points) {
        if(!TVPIsLiveAlphaLayer(nodes,pair.first,pair.second.lifetimeID)) {
            frame->canceled=true; CancelAlphaPointerChain(frame->pointerChain); return true;
        }
    }
    if(frame->presentation && frame->presentation->failed.load(std::memory_order_acquire)) {
        emoteplayer::performanceCounters().alphaFailures.fetch_add(1,std::memory_order_relaxed);
        frame->presentation.reset();
        for(auto& pair:frame->points) { pair.second.snapshot.reset(); pair.second.sourceTexture.reset(); pair.second.layer->Update(); }
        return false;
    }
    bool async=false, ready=true;
    for(auto& pair:frame->points) {
        auto& point=pair.second;
        if(!point.async) continue;
        async=true;
        if(!frame->presentation || !point.snapshot) { ready=false; continue; }
        const auto& request=point.snapshot->read;
        if(frame->presentation->failed.load(std::memory_order_acquire) ||
           (request && (request->failed.load(std::memory_order_acquire) || request->canceled.load(std::memory_order_acquire)))) {
            emoteplayer::performanceCounters().alphaFailures.fetch_add(1,std::memory_order_relaxed);
            frame->presentation.reset();
            for(auto& reset:frame->points) { reset.second.snapshot.reset(); reset.second.sourceTexture.reset(); }
            ready=false; break;
        }
        if(!point.snapshot->Materialize()) { ready=false; continue; }
        point.sourceTexture.reset();
        point.alpha=point.snapshot->alpha[size_t(point.py-point.tile->region.top)*
            point.tile->region.Width()+point.px-point.tile->region.left];
    }
    const bool displayed=!frame->presentation || frame->presentation->presented.load(std::memory_order_acquire);
    if((!async || ready) && displayed) {
        if(async) emoteplayer::performanceCounters().alphaCacheHits.fetch_add(1,std::memory_order_relaxed);
        return true;
    }
    for(auto& pair:frame->points)
        if(pair.second.async && (!frame->presentation || !pair.second.snapshot)) pair.second.layer->Update();
    return false;
}
void tTVPLayerManager::ProcessPendingAlphaInput() {
    if(AlphaDispatching || PendingAlphaInput.empty()) return;
    if(!Primary || !emoteplayer::performanceEnabled("MIKAGE_EMOTE_ASYNC_ALPHA")) {
        PendingAlphaInput.clear(); return;
    }
    // Bound work per host step; event order is preserved across all pointer IDs.
    for(unsigned count=0;count<32 && !PendingAlphaInput.empty();++count) {
        auto frame=PendingAlphaInput.front();
        if(!ResolveAlphaPointer(frame)) break;
        PendingAlphaInput.pop_front(); ActiveAlphaInput=std::move(frame); AlphaDispatching=true;
        if(ActiveAlphaInput->canceled) {
            const auto chain=ActiveAlphaInput->pointerChain;
            ActiveAlphaInput.reset(); AlphaDispatching=false;
            bool retained=false;
            for(const auto& pending:PendingAlphaInput) retained|=pending->pointerChain==chain;
            for(const auto& pointer:AlphaPointerChains) retained|=pointer.second==chain;
            if(!retained) CanceledAlphaPointerChains.erase(chain);
            continue;
        }
        try { ActiveAlphaInput->dispatch(); }
        catch(...) { ActiveAlphaInput.reset(); AlphaDispatching=false; AlphaQuery=false; throw; }
        ActiveAlphaInput.reset(); AlphaDispatching=false; AlphaQuery=false;
    }
}
bool tTVPLayerManager::GetPinnedAlpha(tTJSNI_BaseLayer* layer,tjs_uint32& alpha) const {
    if(!IsAsyncAlphaQuery() || !ActiveAlphaInput) return false;
    auto it=ActiveAlphaInput->points.find(layer);
    if(it==ActiveAlphaInput->points.end() || !it->second.mask ||
       (it->second.async && !it->second.snapshot)) return false;
    alpha=it->second.alpha; return true;
}
bool tTVPLayerManager::GetPinnedHitType(tTJSNI_BaseLayer* layer,tjs_int& hitType) const {
    if(!IsAsyncAlphaQuery() || !ActiveAlphaInput) return false;
    auto it=ActiveAlphaInput->points.find(layer);
    if(it==ActiveAlphaInput->points.end()) return false;
    hitType=it->second.hitType; return true;
}
bool tTVPLayerManager::GetPinnedLayerPoint(tTJSNI_BaseLayer* layer,tjs_int& x,tjs_int& y,bool& inside) const {
    if(!IsAsyncAlphaQuery() || !ActiveAlphaInput) return false;
    auto it=ActiveAlphaInput->points.find(layer);
    if(it==ActiveAlphaInput->points.end()) return false;
    x=it->second.x; y=it->second.y; inside=it->second.inside; return true;
}
bool tTVPLayerManager::HasPinnedLayer(tTJSNI_BaseLayer* layer) const {
    return ActiveAlphaInput && ActiveAlphaInput->points.find(layer)!=ActiveAlphaInput->points.end();
}
void tTVPLayerManager::FromPinnedPrimaryCoordinates(tTJSNI_BaseLayer* layer,tjs_int& x,tjs_int& y) {
    if(AlphaDispatching && ActiveAlphaInput) {
        auto it=ActiveAlphaInput->points.find(layer);
        if(it!=ActiveAlphaInput->points.end()) { x=it->second.x; y=it->second.y; return; }
    }
    layer->FromPrimaryCoordinates(x,y);
}
void tTVPLayerManager::FromPinnedPrimaryCoordinates(tTJSNI_BaseLayer* layer,tjs_real& x,tjs_real& y) {
    if(AlphaDispatching && ActiveAlphaInput) {
        auto it=ActiveAlphaInput->points.find(layer);
        if(it!=ActiveAlphaInput->points.end()) {
            x=it->second.x+(x-ActiveAlphaInput->x); y=it->second.y+(y-ActiveAlphaInput->y); return;
        }
    }
    layer->FromPrimaryCoordinates(x,y);
}
void TVPProcessPendingLayerPointerEvents() {
    auto managers=asyncInputManagers;
    for(auto* manager:managers) manager->AddRef();
    try { for(auto* manager:managers) manager->ProcessPendingAlphaInput(); }
    catch(...) { for(auto* manager:managers) manager->Release(); throw; }
    for(auto* manager:managers) manager->Release();
}
bool TVPHasPendingLayerPointerBackpressure() {
    // SDL's pointer FIFO stays upstream while the GPU is behind. Lifecycle,
    // keyboard and quit events continue through the host's complementary pump.
    for(auto* manager:asyncInputManagers)
        if(manager->PendingAlphaInput.size()>=128) return true;
    return false;
}

//---------------------------------------------------------------------------
// tTVPLayerManager
//---------------------------------------------------------------------------
tTVPLayerManager::tTVPLayerManager(iTVPLayerTreeOwner* owner)
{
    RefCount = 1;
    LayerTreeOwner = owner;
    DrawDeviceData = NULL;
    DrawBuffer = NULL;
    DesiredLayerType = ltOpaque;

    CaptureOwner = NULL;
    LastMouseMoveSent = NULL;
    Primary = NULL;
    FocusedLayer = NULL;
    OverallOrderIndexValid = false;
    EnabledWorkRefCount = 0;
    FocusChangeLock = false;
    VisualStateChanged = true;
    LastMouseMoveX = -1;
    LastMouseMoveY = -1;
    InNotifyingHintOrCursorChange = false;
    asyncInputManagers.push_back(this);
}
//---------------------------------------------------------------------------
tTVPLayerManager::~tTVPLayerManager()
{
    PendingAlphaInput.clear(); ActiveAlphaInput.reset();
    asyncInputManagers.erase(std::remove(asyncInputManagers.begin(),asyncInputManagers.end(),this),asyncInputManagers.end());
    if (DrawBuffer)
        delete DrawBuffer;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::AddRef()
{
    RefCount++;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::Release()
{
    if (RefCount == 1)
        delete this;
    else
        RefCount--;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RegisterSelfToWindow()
{
    PointerPresentationHitTestingEnabled = true;
    LayerTreeOwner->RegisterLayerManager(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::UnregisterSelfFromWindow()
{
    PointerPresentationHitTestingEnabled = true;
    LayerTreeOwner->UnregisterLayerManager(this);
}

void tTVPLayerManager::SetHoldAlpha(bool b)
{
    HoldAlpha = b;
    if (!DrawBuffer)
        return;
    static_cast<tTVPDestTexture*>(DrawBuffer)->SetHoldAlpha(b);
}

//---------------------------------------------------------------------------
tTVPBaseTexture* tTVPLayerManager::GetDrawTargetBitmap(const tTVPRect& rect, tTVPRect& cliprect)
{
    // retrieve draw target bitmap
    tjs_int w = rect.get_width();
    tjs_int h = rect.get_height();

    if (!DrawBuffer)
    {
        // create draw buffer
        if (Primary)
        {
            const tTVPRect& rc = Primary->GetRect();
            w = rc.get_width();
            h = rc.get_height();
        }
        DrawBuffer = new tTVPDestTexture(w, h);
        DrawBuffer->Fill(tTVPRect(0, 0, w, h), 0xFF000000);
        static_cast<tTVPDestTexture*>(DrawBuffer)->SetHoldAlpha(HoldAlpha);
    }
    else
    {
        tjs_int bw = DrawBuffer->GetWidth();
        tjs_int bh = DrawBuffer->GetHeight();
        if (bw < w || bh < h)
        {
            // insufficient size; resize the draw buffer
            tjs_uint neww = bw > w ? bw : w, newh = bh > h ? bh : h;
            neww += (neww & 1); // align to even
            DrawBuffer->SetSize(neww, newh, false);
            DrawBuffer->Fill(tTVPRect(0, 0, neww, newh), 0xFF000000);
        }
    }

    cliprect = rect;
    return DrawBuffer;
}
//---------------------------------------------------------------------------
tTVPLayerType tTVPLayerManager::GetTargetLayerType()
{
    return DesiredLayerType;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DrawCompleted(const tTVPRect& destrect,
                                     tTVPBaseTexture* bmp,
                                     const tTVPRect& cliprect,
                                     tTVPLayerType type,
                                     tjs_int opacity)
{
    tjs_int w, h;
    if (!/*LayerTreeOwner->*/ GetPrimaryLayerSize(w, h))
        return;
    // Window->GetDrawDevice()->GetSrcSize(w, h);
    if (!DrawBuffer)
    {
        // create draw buffer
        DrawBuffer = new tTVPDestTexture(w, h);
        DrawBuffer->Fill(tTVPRect(0, 0, w, h), 0xFF000000);
        static_cast<tTVPDestTexture*>(DrawBuffer)->SetHoldAlpha(HoldAlpha);
    }
    else
    {
        tjs_int bw = DrawBuffer->GetWidth();
        tjs_int bh = DrawBuffer->GetHeight();
        if (bw < w || bh < h)
        {
            // insufficient size; resize the draw buffer
            tjs_uint neww = bw > w ? bw : w, newh = bh > h ? bh : h;
            neww += (neww & 1); // align to even
            DrawBuffer->SetSize(neww, newh, false);
            DrawBuffer->Fill(tTVPRect(0, 0, neww, newh), 0xFF000000);
        }
    }

    DrawBuffer->Blt(destrect.left, destrect.top, bmp, cliprect, type, opacity, HoldAlpha);
}

tTVPBaseTexture* tTVPLayerManager::GetOrCreateDrawBuffer()
{
    if (!DrawBuffer)
    {
        tjs_int w, h;
        if (!GetPrimaryLayerSize(w, h))
            return nullptr;
        DrawBuffer = new tTVPDestTexture(w, h);
        DrawBuffer->Fill(tTVPRect(0, 0, w, h), 0xFF000000);
        static_cast<tTVPDestTexture*>(DrawBuffer)->SetHoldAlpha(HoldAlpha);
    }
    return DrawBuffer;
}

//---------------------------------------------------------------------------
void tTVPLayerManager::AttachPrimary(tTJSNI_BaseLayer* pri)
{
    // attach primary layer to the manager
    DetachPrimary();

    if (!Primary)
    {
        Primary = pri;
        EnabledWorkRefCount = 0;
        OverallOrderIndexValid = false;
        UpdateRegion.Clear();
        pri->SetVisible(true);
        pri->SetOpacity(255);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DetachPrimary()
{
    PendingAlphaInput.clear();
    AlphaPointerChains.clear(); CanceledAlphaPointerChains.clear();
    // detach primary layer from the manager
    if (Primary)
    {
        SetFocusTo(NULL);
        ReleaseCapture();
        ReleaseTouchCaptureAll();
        ForceMouseLeave();
        NotifyPart(Primary);
        Primary = NULL;
    }
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::GetPrimaryLayerSize(tjs_int& w, tjs_int& h) const
{
    if (IsPrimaryLayerAttached())
    {
        w = Primary->GetWidth();
        h = Primary->GetHeight();
        return true;
    }
    else
    {
        return false;
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyPart(tTJSNI_BaseLayer* lay)
{
    // notifies layer parting from its parent
    InvalidateOverallIndex();
    BlurTree(lay);
    ReleaseCaptureFromTree(lay);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::InvalidateOverallIndex()
{
    OverallOrderIndexValid = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RecreateOverallOrderIndex()
{
    // recreate overall order index
    if (!OverallOrderIndexValid)
    {
        tjs_uint index = 0;
        AllNodes.clear();
        if (Primary)
            Primary->RecreateOverallOrderIndex(index, AllNodes);

        OverallOrderIndexValid = true;
    }
}
//---------------------------------------------------------------------------
std::vector<tTJSNI_BaseLayer*>& tTVPLayerManager::GetAllNodes()
{
    if (!OverallOrderIndexValid)
        RecreateOverallOrderIndex();
    return AllNodes;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::QueryUpdateExcludeRect()
{
    if (!VisualStateChanged)
        return;
    tTVPRect r;
    r.clear();
    if (Primary)
        Primary->QueryUpdateExcludeRect(r, true);
    VisualStateChanged = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyMouseCursorChange(tTJSNI_BaseLayer* layer, tjs_int cursor)
{
    if (!PointerPresentationHitTestingEnabled || InNotifyingHintOrCursorChange)
        return;

    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::CursorChange, false);

    InNotifyingHintOrCursorChange = true;
    try
    {

        tTJSNI_BaseLayer* l;

        if (CaptureOwner)
            l = CaptureOwner;
        else
            l = GetMostFrontChildAt(LastMouseMoveX, LastMouseMoveY);

        if (l == layer)
            SetMouseCursor(cursor);
    }
    catch (...)
    {
        InNotifyingHintOrCursorChange = false;
        throw;
    }

    InNotifyingHintOrCursorChange = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetMouseCursor(tjs_int cursor)
{
    if (!LayerTreeOwner)
        return;

    LayerTreeOwner->SetMouseCursor(this, cursor);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::GetCursorPos(tjs_int& x, tjs_int& y)
{
    if (!LayerTreeOwner)
        return;
    LayerTreeOwner->GetCursorPos(this, x, y);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetCursorPos(tjs_int x, tjs_int y)
{
    if (!LayerTreeOwner)
        return;
    LayerTreeOwner->SetCursorPos(this, x, y);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyHintChange(tTJSNI_BaseLayer* layer, const ttstr& hint)
{
    if (!PointerPresentationHitTestingEnabled || InNotifyingHintOrCursorChange)
        return;

    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::HintChange, false);

    InNotifyingHintOrCursorChange = true;

    try
    {
        tTJSNI_BaseLayer* l;

        if (CaptureOwner)
            l = CaptureOwner;
        else
            l = GetMostFrontChildAt(LastMouseMoveX, LastMouseMoveY);

        if (l == layer)
            SetHint(l->GetOwnerNoAddRef(), hint);
    }
    catch (...)
    {
        InNotifyingHintOrCursorChange = false;
        throw;
    }

    InNotifyingHintOrCursorChange = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetHint(iTJSDispatch2* sender, const ttstr& hint)
{
    if (!LayerTreeOwner)
        return;
    LayerTreeOwner->SetHint(this, sender, hint);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyLayerResize()
{
    // notifies layer resizing to the LayerTreeOwner
    if (!LayerTreeOwner)
        return;

    LayerTreeOwner->NotifyLayerResize(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyWindowInvalidation()
{
    // notifies layer surface is invalidated and should be transfered to LayerTreeOwner.
    if (!LayerTreeOwner)
        return;

    LayerTreeOwner->NotifyLayerImageChange(this);
    // TODO atlernative of LayerTreeOwner->RequestUpdate();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetLayerTreeOwner(class iTVPLayerTreeOwner* owner)
{
    // sets LayerTreeOwner
    PointerPresentationHitTestingEnabled = true;
    LayerTreeOwner = owner;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyResizeFromWindow(tjs_uint w, tjs_uint h)
{
    // is called by the owner window, notifies windows's client area size
    // has been changed.
    // does not be called if owner window's "autoResize" property is false.

    // currently this function is not used.

    if (Primary)
        Primary->InternalSetSize(w, h);
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer* tTVPLayerManager::GetMostFrontChildAt(tjs_int x,
                                                        tjs_int y,
                                                        tTJSNI_BaseLayer* except,
                                                        bool get_disabled)
{
    // return most front layer at given point.
    // this does checking of layer's visibility.
    // x and y are given in primary layer's coordinates.
    if (!Primary)
        return NULL;

    tTJSNI_BaseLayer* lay = NULL;
    const bool priorQuery=AlphaQuery;
    AlphaQuery=AlphaDispatching;
    try { Primary->GetMostFrontChildAt(x, y, &lay, except, get_disabled); }
    catch(...) { AlphaQuery=priorQuery; throw; }
    AlphaQuery=priorQuery;
    return lay;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryClick(tjs_int x, tjs_int y)
{
    AlphaPointerScope alphaScope(*this,x,y,[this,x,y]{ PrimaryClick(x,y); });
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::Click);
    tTJSNI_BaseLayer* l = GetMostFrontChildAt(x, y);
    if (l && CaptureOwner == l)
    {
        FromPinnedPrimaryCoordinates(l,x,y);
        l->FireClick(x, y);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryDoubleClick(tjs_int x, tjs_int y)
{
    AlphaPointerScope alphaScope(*this,x,y,[this,x,y]{ PrimaryDoubleClick(x,y); });
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::DoubleClick);
    tTJSNI_BaseLayer* l = GetMostFrontChildAt(x, y);
    if (l /*&& CaptureOwner == l*/)
    {
        FromPinnedPrimaryCoordinates(l,x,y);
        l->FireDoubleClick(x, y);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseDown(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
    AlphaPointerScope alphaScope(*this,x,y,[this,x,y,mb,flags]{ PrimaryMouseDown(x,y,mb,flags); },0,true);
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::PointerDown);
    PrimaryMouseMove(x, y, flags);
    tTJSNI_BaseLayer* l = CaptureOwner ? CaptureOwner : GetMostFrontChildAt(x, y);
    if (l)
    {
        FromPinnedPrimaryCoordinates(l,x,y);
        ReleaseCaptureCalled = false;
        l->FireMouseDown(x, y, mb, flags);
        bool no_capture = ReleaseCaptureCalled;

        if (CaptureOwner != l)
        {
            ReleaseCapture();

            if (!no_capture)
            {
                CaptureOwner = l;
                if (CaptureOwner->Owner)
                    CaptureOwner->Owner->AddRef(); // addref TJS object
            }
        }

        SetHint(NULL, ttstr());
    }
    else
    {
        ReleaseCapture();
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseUp(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
    AlphaPointerScope alphaScope(*this,x,y,[this,x,y,mb,flags]{ PrimaryMouseUp(x,y,mb,flags); });
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::PointerUp);
    tTJSNI_BaseLayer* l;

    if (CaptureOwner)
        l = CaptureOwner;
    else
        l = GetMostFrontChildAt(x, y);

    if (l)
    {
        int orig_x = x, orig_y = y;

        FromPinnedPrimaryCoordinates(l,x,y);
        l->FireMouseUp(x, y, mb, flags);

        if (!TVPIsAnyMouseButtonPressedInShiftStateFlags(flags))
        {
            ReleaseCapture();
            PrimaryMouseMove(orig_x, orig_y, flags); // force recheck current under-cursor layer
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseMove(tjs_int x, tjs_int y, tjs_uint32 flags)
{
    AlphaPointerScope alphaScope(*this,x,y,[this,x,y,flags]{ PrimaryMouseMove(x,y,flags); });
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::PointerMove);
    bool poschanged = (LastMouseMoveX != x || LastMouseMoveY != y);
    LastMouseMoveX = x;
    LastMouseMoveY = y;

    tTJSNI_BaseLayer* l;

    if (CaptureOwner)
        l = CaptureOwner;
    else
        l = GetMostFrontChildAt(x, y);

    // enter/leave event
    if (LastMouseMoveSent != l)
    {
        if (LastMouseMoveSent)
            LastMouseMoveSent->FireMouseLeave();

        // recheck l because the layer may become invalid during
        // FireMouseLeave call.
        if (CaptureOwner)
            l = CaptureOwner;
        else
            l = GetMostFrontChildAt(x, y);

        if (l)
        {
            InNotifyingHintOrCursorChange = true;
            try
            {
                tTJSNI_BaseLayer* ll;

                l->FireMouseEnter();

                // recheck l because the layer may become invalid during
                // FireMouseEnter call.
                if (CaptureOwner)
                    ll = CaptureOwner;
                else
                    ll = GetMostFrontChildAt(x, y);

                if (l != ll)
                {
                    l->FireMouseLeave();
                    l = ll;
                    if (l)
                        l->FireMouseEnter();
                }

                // note: rechecking is done only once to avoid infinite loop

                if (l)
                    l->SetCurrentCursorToWindow();
                if (l)
                    l->SetCurrentHintToWindow();
            }
            catch (...)
            {
                InNotifyingHintOrCursorChange = false;
                throw;
            }
            InNotifyingHintOrCursorChange = false;
        }

        if (!l)
        {
            SetMouseCursor(0);
            SetHint(NULL, ttstr());
        }
    }

    if (LastMouseMoveSent != l)
    {
        if (LastMouseMoveSent)
        {
            tTJSNI_BaseLayer* lay = LastMouseMoveSent;
            LastMouseMoveSent = NULL;
            if (lay->Owner)
                lay->Owner->Release();
        }

        LastMouseMoveSent = l;

        if (LastMouseMoveSent)
        {
            if (LastMouseMoveSent->Owner)
                LastMouseMoveSent->Owner->AddRef();
        }
    }

    if (l)
    {
        if (poschanged)
        {
            FromPinnedPrimaryCoordinates(l,x,y);
            l->FireMouseMove(x, y, flags);
        }
    }
    else
    {
        // no layer to send the event
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchDown(
    tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id)
{
    AlphaPointerScope alphaScope(*this,tjs_int(x),tjs_int(y),[this,x,y,cx,cy,id]{ PrimaryTouchDown(x,y,cx,cy,id); },uint64_t(id)+1,true);
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::PointerDown);
    tjs_int ix = (tjs_int)x, iy = (tjs_int)y;
    ReleaseTouchCapture(id);
    tTJSNI_BaseLayer* l = GetMostFrontChildAt(ix, iy);
    if (l)
    {
        FromPinnedPrimaryCoordinates(l,x,y);
        ReleaseTouchCaptureIDMark = (tjs_int64)id;
        l->FireTouchDown(x, y, cx, cy, id);
        if (ReleaseTouchCaptureIDMark == (tjs_int64)id)
        {
            SetTouchCapture(id, l);
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchUp(
    tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id)
{
    AlphaPointerScope alphaScope(*this,tjs_int(x),tjs_int(y),[this,x,y,cx,cy,id]{ PrimaryTouchUp(x,y,cx,cy,id); },uint64_t(id)+1,false,true);
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::PointerUp);
    tjs_int ix = (tjs_int)x, iy = (tjs_int)y;
    tTJSNI_BaseLayer* l = GetTouchCapture(id) ? GetTouchCapture(id) : GetMostFrontChildAt(ix, iy);
    if (l)
    {
        FromPinnedPrimaryCoordinates(l,x,y);
        l->FireTouchUp(x, y, cx, cy, id);
        ReleaseTouchCapture(id);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchMove(
    tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id)
{
    AlphaPointerScope alphaScope(*this,tjs_int(x),tjs_int(y),[this,x,y,cx,cy,id]{ PrimaryTouchMove(x,y,cx,cy,id); },uint64_t(id)+1,false);
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::PointerMove);
    tjs_int ix = (tjs_int)x, iy = (tjs_int)y;
    tTJSNI_BaseLayer* l = GetTouchCapture(id) ? GetTouchCapture(id) : GetMostFrontChildAt(ix, iy);
    if (l)
    {
        FromPinnedPrimaryCoordinates(l,x,y);
        l->FireTouchMove(x, y, cx, cy, id);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchScaling(
    tjs_real startdist, tjs_real curdist, tjs_real cx, tjs_real cy, tjs_int flag)
{
    AlphaPointerScope alphaScope(*this,tjs_int(cx),tjs_int(cy),[this,startdist,curdist,cx,cy,flag]{ PrimaryTouchScaling(startdist,curdist,cx,cy,flag); });
    if(alphaScope.Deferred()) return;
    if (FocusedLayer)
        FocusedLayer->FireTouchScaling(startdist, curdist, cx, cy, flag);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchRotate(
    tjs_real startangle, tjs_real curangle, tjs_real dist, tjs_real cx, tjs_real cy, tjs_int flag)
{
    AlphaPointerScope alphaScope(*this,tjs_int(cx),tjs_int(cy),[this,startangle,curangle,dist,cx,cy,flag]{ PrimaryTouchRotate(startangle,curangle,dist,cx,cy,flag); });
    if(alphaScope.Deferred()) return;
    if (FocusedLayer)
        FocusedLayer->FireTouchRotate(startangle, curangle, dist, cx, cy, flag);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMultiTouch()
{
    AlphaPointerScope alphaScope(*this,LastMouseMoveX,LastMouseMoveY,[this]{ PrimaryMultiTouch(); });
    if(alphaScope.Deferred()) return;
    if (FocusedLayer)
        FocusedLayer->FireMultiTouch();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ForceMouseLeave()
{
    if (LastMouseMoveSent)
    {
        tTJSNI_BaseLayer* lay = LastMouseMoveSent;
        LastMouseMoveSent = NULL;
        lay->FireMouseLeave();
        if (lay->Owner)
            lay->Owner->Release();
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ForceMouseRecheck()
{
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::InputRecheck);
    PrimaryMouseMove(LastMouseMoveX, LastMouseMoveY, 0);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::MouseOutOfWindow()
{
    // notifys that the mouse cursor goes outside of the window.
    if (!CaptureOwner)
        PrimaryMouseMove(-1, -1, 0); // force mouse cursor out of the all
}
//---------------------------------------------------------------------------
void tTVPLayerManager::LeaveMouseFromTree(tTJSNI_BaseLayer* root)
{
    // force to leave mouse
    if (LastMouseMoveSent)
    {
        if (LastMouseMoveSent->IsAncestorOrSelf(root))
        {
            tTJSNI_BaseLayer* lay = LastMouseMoveSent;
            LastMouseMoveSent = NULL;
            lay->FireMouseLeave();
            if (lay->Owner)
                lay->Owner->Release();
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseCapture()
{
    // release capture state
    ReleaseCaptureCalled = true;
    if (CaptureOwner)
    {
        tTJSNI_BaseLayer* lay = CaptureOwner;
        CaptureOwner = NULL;
        if (lay->Owner)
            lay->Owner->Release();
        // release TJS object

        LayerTreeOwner->ReleaseMouseCapture(this);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseCaptureFromTree(tTJSNI_BaseLayer* layer)
{
    // Release capture state, if the capture object is descendant of
    // 'layer' or 'layer' itself.
    if (CaptureOwner)
    {
        if (CaptureOwner->IsAncestorOrSelf(layer))
        {
            ReleaseCapture();
        }
    }
    std::vector<tTVPTouchCaptureLayer>::iterator itr = TouchCapture.begin();
    while (itr != TouchCapture.end())
    {
        tTJSNI_BaseLayer* l = itr->Owner;
        if (l && l->IsAncestorOrSelf(layer))
        {
            if (l->Owner)
                l->Owner->Release();
            if (ReleaseTouchCaptureIDMark == (tjs_int64)(itr->TouchID))
                ReleaseTouchCaptureIDMark = -1;
            itr = TouchCapture.erase(itr);
        }
        else
        {
            itr++;
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseTouchCapture(tjs_uint32 id)
{
    FindTouchID pred(id);
    std::vector<tTVPTouchCaptureLayer>::iterator itr =
        std::find_if(TouchCapture.begin(), TouchCapture.end(), pred);
    if (itr != TouchCapture.end())
    {
        tTJSNI_BaseLayer* old = itr->Owner;
        if (old && old->Owner)
            old->Owner->Release();
        TouchCapture.erase(itr);
    }
    if (ReleaseTouchCaptureIDMark == (tjs_int64)id)
        ReleaseTouchCaptureIDMark = -1;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseTouchCaptureAll()
{
    for (std::vector<tTVPTouchCaptureLayer>::iterator itr = TouchCapture.begin();
         itr != TouchCapture.end(); itr++)
    {
        tTJSNI_BaseLayer* l = itr->Owner;
        if (l->Owner)
            l->Owner->Release();
    }
    TouchCapture.clear();
    ReleaseTouchCaptureIDMark = -1;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetTouchCapture(tjs_uint32 id, tTJSNI_BaseLayer* layer)
{
    FindTouchID pred(id);
    std::vector<tTVPTouchCaptureLayer>::iterator itr =
        std::find_if(TouchCapture.begin(), TouchCapture.end(), pred);
    if (itr != TouchCapture.end())
    {
        // 既に同一IDのものがある場合は、同じ場所で置き換える
        tTJSNI_BaseLayer* old = itr->Owner;
        if (old && old->Owner)
            old->Owner->Release();
        itr->Owner = layer;
        if (layer->Owner)
            layer->Owner->AddRef();
    }
    else
    {
        // ない場合は、末尾に追加。
        TouchCapture.push_back(tTVPTouchCaptureLayer(id, layer));
        if (layer->Owner)
            layer->Owner->AddRef();
    }
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::BlurTree(tTJSNI_BaseLayer* root)
{
    // (primary only) remove focus from "root"
    RemoveTreeModalState(root);
    LeaveMouseFromTree(root);

    if (!FocusedLayer)
        return false;

    if (!FocusedLayer->IsAncestorOrSelf(root))
        return false;
    // root is not ancestor of current focused layer

    tTJSNI_BaseLayer* next = root->GetNextFocusable();

    if (next != FocusedLayer)
        SetFocusTo(next, true); // focus to root's next focusable layer
    else
        SetFocusTo(NULL, true);

    return true;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::CheckTreeFocusableState(tTJSNI_BaseLayer* root)
{
    // (primary only) check newly added tree's focusable state
    /*	// uncomment here to auto-focus
            if(FocusedLayer) return;

            tTJSNI_BaseLayer *lay = root->SearchFirstFocusable(true);
            if(lay) SetFocusTo(lay, true);
    */
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer* tTVPLayerManager::FocusPrev()
{
    // focus to previous layer
    tTJSNI_BaseLayer* l;
    if (!FocusedLayer)
        l = SearchFirstFocusable(false); // search first focusable layer
    else
        l = FocusedLayer->GetPrevFocusable();

    if (l)
        SetFocusTo(l, false);
    return l;
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer* tTVPLayerManager::FocusNext()
{
    // focus to next layer
    tTJSNI_BaseLayer* l;
    if (!FocusedLayer)
        l = SearchFirstFocusable(false); // search first focusable layer
    else
        l = FocusedLayer->GetNextFocusable();

    if (l)
        SetFocusTo(l, true);
    return l;
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer* tTVPLayerManager::SearchFirstFocusable(bool ignore_chain_focusable)
{
    // (primary only) search first focusable layer
    if (!Primary)
        return NULL;
    tTJSNI_BaseLayer* lay = Primary->SearchFirstFocusable(ignore_chain_focusable);

    return lay;
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::SetFocusTo(tTJSNI_BaseLayer* layer, bool direction)
{
    // set focus to layer

    // direction = true : forward focus
    // direction = false: backward focus

    if (layer && !layer->GetNodeFocusable())
        return false;

    if (layer && !layer->Shutdown)
        layer = layer->FireBeforeFocus(FocusedLayer, direction);

    if (layer && !layer->GetNodeFocusable())
        return false;

    if (FocusedLayer == layer)
        return false;

    if (FocusChangeLock)
        TVPThrowExceptionMessage(TVPCannotChangeFocusInProcessingFocus);
    FocusChangeLock = true;

    tTJSNI_BaseLayer* org = FocusedLayer;
    FocusedLayer = layer;

    try
    {
        if (org && !org->Shutdown)
            org->FireBlur(layer);

        if (FocusedLayer && !FocusedLayer->Shutdown)
            FocusedLayer->FireFocus(org, direction);
    }
    catch (...)
    {
        if (FocusedLayer)
            if (FocusedLayer->Owner)
                FocusedLayer->Owner->AddRef();
        if (org)
            if (org->Owner)
                org->Owner->Release();
        FocusChangeLock = false;
        throw;
    }

    if (FocusedLayer)
        if (FocusedLayer->Owner)
            FocusedLayer->Owner->AddRef();
    if (org)
        if (org->Owner)
            org->Owner->Release();

    if (FocusedLayer)
        SetImeModeOf(FocusedLayer);
    else
        ResetImeMode();
    if (FocusedLayer)
        SetAttentionPointOf(FocusedLayer);
    else
        DisableAttentionPoint();

    FocusChangeLock = false;
    return true;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseAllModalLayer()
{
    // (primary only) release all modal layer on invalidation
    std::vector<tTJSNI_BaseLayer*> copy(ModalLayerVector);
    ModalLayerVector.clear();

    std::vector<tTJSNI_BaseLayer*>::iterator i;
    for (i = copy.begin(); i < copy.end(); i++)
    {
        if ((*i)->Owner)
            (*i)->Owner->Release();
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetModeTo(tTJSNI_BaseLayer* layer)
{
    // (primary only) set mode to layer
    if (!layer)
        return;

    SaveEnabledWork();

    try
    {
        tTJSNI_BaseLayer* current = GetCurrentModalLayer();
        if (current && layer->IsAncestorOrSelf(current))
            TVPThrowExceptionMessage(TVPCannotSetModeToDisabledOrModal);
        // cannot set mode to parent layer
        if (!layer->Visible)
            layer->Visible = true;
        if (!layer->GetParentVisible() || !layer->Enabled)
            TVPThrowExceptionMessage(TVPCannotSetModeToDisabledOrModal);
        // cannot set mode to parent layer
        if (layer == current)
            TVPThrowExceptionMessage(TVPCannotSetModeToDisabledOrModal);
        // cannot set mode to already modal layer

        SetFocusTo(layer->SearchFirstFocusable(), true);

        if (layer->Owner)
            layer->Owner->AddRef();
        ModalLayerVector.push_back(layer);
    }
    catch (...)
    {
        NotifyNodeEnabledState();
        throw;
    }

    NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RemoveModeFrom(tTJSNI_BaseLayer* layer)
{
    // remove modal state from given layer
    bool do_notify = false;

    try
    {
        std::vector<tTJSNI_BaseLayer*>::iterator i;
        for (i = ModalLayerVector.begin(); i < ModalLayerVector.end();)
        {
            if (layer == *i)
            {
                if (!do_notify)
                {
                    do_notify = true;
                    SaveEnabledWork();
                }
                if (layer->Owner)
                    layer->Owner->Release();
                SetFocusTo(layer->GetNextFocusable(), true);
                i = ModalLayerVector.erase(i);
            }
            else
            {
                i++;
            }
        }
    }
    catch (...)
    {
        if (do_notify)
            NotifyNodeEnabledState();
        throw;
    }

    if (do_notify)
        NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RemoveTreeModalState(tTJSNI_BaseLayer* root)
{
    // remove modal state from given tree
    bool do_notify = false;

    try
    {
        std::vector<tTJSNI_BaseLayer*>::iterator i;
        for (i = ModalLayerVector.begin(); i < ModalLayerVector.end();)
        {
            if ((*i)->IsAncestorOrSelf(root))
            {
                if (!do_notify)
                {
                    do_notify = true;
                    SaveEnabledWork();
                }
                if ((*i)->Owner)
                    (*i)->Owner->Release();
                SetFocusTo(root->GetNextFocusable(), true);
                i = ModalLayerVector.erase(i);
            }
            else
            {
                i++;
            }
        }
    }
    catch (...)
    {
        if (do_notify)
            NotifyNodeEnabledState();
        throw;
    }

    if (do_notify)
        NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer* tTVPLayerManager::GetCurrentModalLayer() const
{
    // (primary only) get current modal layer
    tjs_uint size = (tjs_uint)ModalLayerVector.size();
    if (size == 0)
        return NULL;
    return *(ModalLayerVector.begin() + size - 1);
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::SearchAttentionPoint(tTJSNI_BaseLayer* target, tjs_int& x, tjs_int& y)
{
    // search specified layer 's attention point
    while (target)
    {
        if (target->UseAttention)
        {
            x = target->AttentionLeft, y = target->AttentionTop;
            target->ToPrimaryCoordinates(x, y);
            return true;
        }
        target = target->Parent;
    }
    return false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetAttentionPointOf(tTJSNI_BaseLayer* layer)
{
    if (!LayerTreeOwner)
        return;
    tjs_int x, y;
    if (SearchAttentionPoint(layer, x, y))
        LayerTreeOwner->SetAttentionPoint(this, layer, x, y);
    else
        LayerTreeOwner->DisableAttentionPoint(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DisableAttentionPoint()
{
    if (LayerTreeOwner)
        LayerTreeOwner->DisableAttentionPoint(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyAttentionStateChanged(tTJSNI_BaseLayer* from)
{
    if (FocusedLayer == from)
    {
        SetAttentionPointOf(from);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetImeModeOf(tTJSNI_BaseLayer* layer)
{
    if (!LayerTreeOwner)
        return;
    LayerTreeOwner->SetImeMode(this, layer->ImeMode);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ResetImeMode()
{
    if (!LayerTreeOwner)
        return;
    LayerTreeOwner->ResetImeMode(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyImeModeChanged(tTJSNI_BaseLayer* from)
{
    if (FocusedLayer == from)
    {
        SetImeModeOf(from);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SaveEnabledWork()
{
    // save current node enabled state to EnabledWork
    // this does recursive call
    if (EnabledWorkRefCount == 0)
        if (Primary)
            Primary->SaveEnabledWork();

    EnabledWorkRefCount++;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyNodeEnabledState()
{
    // notify node enabled state change to self and its children
    // this refers EnabledWork which is created by SaveEnabledWork
    EnabledWorkRefCount--;

    if (EnabledWorkRefCount == 0)
        if (Primary)
            Primary->NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryKeyDown(tjs_uint key, tjs_uint32 shift)
{
    if (FocusedLayer)
        FocusedLayer->FireKeyDown(key, shift);
    else if (Primary)
        Primary->DefaultKeyDown(key, shift);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryKeyUp(tjs_uint key, tjs_uint32 shift)
{
    if (FocusedLayer)
        FocusedLayer->FireKeyUp(key, shift);
    else if (Primary)
        Primary->DefaultKeyUp(key, shift);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryKeyPress(tjs_uint16 key)
{
    if (FocusedLayer)
        FocusedLayer->FireKeyPress(key);
    else if (Primary)
        Primary->DefaultKeyPress(key);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseWheel(tjs_uint32 shift, tjs_int delta, tjs_int x, tjs_int y)
{
    AlphaPointerScope alphaScope(*this,x,y,[this,shift,delta,x,y]{ PrimaryMouseWheel(shift,delta,x,y); });
    if(alphaScope.Deferred()) return;
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::Wheel);
    if (FocusedLayer)
        FocusedLayer->FireMouseWheel(shift, delta, x, y);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::AddUpdateRegion(const tTVPComplexRect& rects)
{
    UpdateRegion.Or(rects);
    if (UpdateRegion.GetCount() > TVP_UPDATE_UNITE_LIMIT)
        UpdateRegion.Unite();
    NotifyWindowInvalidation();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::AddUpdateRegion(const tTVPRect& rect)
{
    // the window is invalidated;
    UpdateRegion.Or(rect);
    NotifyWindowInvalidation();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::UpdateToDrawDevice()
{
    // drawdevice -> layer
    if (!Primary)
        return;
    TVPBeginEmoteAlphaPresentation();
    struct EndAlphaPresentation { ~EndAlphaPresentation() { TVPEndEmoteAlphaPresentation(); } } endAlphaPresentation;
    BindAlphaPresentation();
    // Retry older frozen frames before new source reads can claim the bounded
    // budget. Continually arriving pointer events must not starve FIFO's head.
    FinishAlphaPresentation();
    Primary->CompleteForWindow(this);
    if(emoteplayer::performanceEnabled("MIKAGE_EMOTE_ASYNC_ALPHA")) {
        // Snapshot only a real window composition, never an arbitrary script
        // GetTextureHandle. Pending candidates force their own Update first.
        for(auto* layer:GetAllNodes()) {
            if(!layer || !layer->GetNodeVisible()) continue;
            auto* image=layer->GetMainImage();
            CaptureAlphaForPresentation(layer);
            if(image) TVPEncodeEmoteAsyncAlphaForPresentation(image->GetTexture());
        }
        FinishAlphaPresentation();
        if(PendingAlphaInput.empty()) for(auto* layer:GetAllNodes()) {
            if(auto* image=layer->GetMainImage()) TVPStopEmoteAsyncAlphaDemand(image->GetTexture());
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyUpdateRegionFixed()
{
    // called by primary layer, notifying final update region is fixed
    //	Window->NotifyUpdateRegionFixed(UpdateRegion);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RequestInvalidation(const tTVPRect& r)
{
    // called by the owner window to notify window surface is invalidated by
    // the system or user.
    if (!Primary)
        return;

    tTVPRect ur;
    tTVPRect cr(0, 0, Primary->Rect.get_width(), Primary->Rect.get_height());

    if (TVPIntersectRect(&ur, r, cr))
    {
        AddUpdateRegion(ur);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RecheckInputState()
{
    krkrsdl3::point_trace::TriggerScope trace(krkrsdl3::point_trace::Trigger::InputRecheck, false);
    // To re-check current layer under current mouse position
    // and update hint, cursor type and process layer enter/leave.
    // This can be reasonably slow, about 1 sec interval.
    ForceMouseRecheck();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DumpLayerStructure()
{
    if (Primary)
        Primary->DumpStructure();
}
//---------------------------------------------------------------------------

bool tTVPDestTexture::CopyRect(tjs_int x,
                               tjs_int y,
                               const iTVPBaseBitmap* ref,
                               tTVPRect refrect,
                               tjs_int plane /*= (TVP_BB_COPY_MAIN | TVP_BB_COPY_MASK)*/)
{
    if (HoldAlpha)
    {
        return tTVPBaseTexture::CopyRect(x, y, ref, refrect, TVP_BB_COPY_MAIN);
    }
    else
    {
        return tTVPBaseTexture::CopyRect(x, y, ref, refrect, plane);
    }
}
