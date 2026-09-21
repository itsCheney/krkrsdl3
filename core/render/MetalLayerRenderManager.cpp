#include "tjsCommHead.h"
#include "RenderManager.h"
#include "MetalLayerRenderManager.h"
#include "LayerBitmap.h"
#include "TVPCompositor.h"
#include "gl/tvpgl.h"
#include "Platform.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[65536];
extern unsigned char TVPNegativeMulTable[65536];
}

namespace {
using krkrsdl3::iTVPRenderBackend;
class LayerTexture;
std::string fallbackReason;
struct Session {
    iTVPRenderBackend* backend;
    TVPLayerRenderStats stats;
    std::set<LayerTexture*> textures;
    bool tablesReady = false;
    explicit Session(iTVPRenderBackend* b) : backend(b) {}
};
TVPLayerRect Rect(const tTVPRect& r) { return {r.left,r.top,r.right,r.bottom}; }
class LayerTexture final : public iTVPTexture2D {
    std::shared_ptr<Session> session;
    TVPTextureFormat::e format;
    void* handle = nullptr;
    std::vector<uint8_t> pixels;
    bool valid = false, dirty = false, pinned = false, readonly;
    // An outstanding raw write pointer may be written at any time without
    // telling us, so such a texture re-uploads until the lease is released.
    bool writeLeased = false;
    unsigned locks = 0;
    // Union of CPU writes since the last upload; meaningful only while dirty.
    tTVPRect damage;
    // Damage outstanding when the current write lease opened. A lease widens
    // damage to the whole surface while it is held, so releasing it must restore
    // this rather than drop writes the lease did not make.
    bool leaseHadDamage = false;
    tTVPRect leaseDamage;
    size_t Bytes() const { return size_t(GetPitch())*Height; }
    // Writers report what they touched so an upload carries only those rows.
    // Callers that cannot describe their writes report the whole surface.
    void MarkDirty(const tTVPRect& requested) {
        tTVPRect r(std::max(0,requested.left),std::max(0,requested.top),
                   std::min(int(Width),requested.right),std::min(int(Height),requested.bottom));
        if(r.get_width()<=0 || r.get_height()<=0) return;
        if(!dirty) { damage=r; dirty=true; return; }
        damage.left=std::min(damage.left,r.left); damage.top=std::min(damage.top,r.top);
        damage.right=std::max(damage.right,r.right); damage.bottom=std::max(damage.bottom,r.bottom);
    }
    void MarkDirtyAll() { MarkDirty(tTVPRect(0,0,Width,Height)); }
    void Read() {
        if(valid) return;
        if(!session->backend || !handle) throw std::runtime_error("GPU Layer read without a session");
        int pitch=0;
        if(!session->backend->ReadLayerTexture(handle,pixels,pitch) || pitch!=GetPitch())
            throw std::runtime_error("GPU Layer readback failed");
        session->stats.readbackBytes+=Bytes(); session->stats.cpuCacheBytes+=Bytes(); valid=true;
    }
public:
    LayerTexture(std::shared_ptr<Session> s,unsigned w,unsigned h,TVPTextureFormat::e f,bool ro)
        : iTVPTexture2D(w,h),session(std::move(s)),format(f),readonly(ro) {
        handle=session->backend->CreateLayerTexture(w,h,f==TVPTextureFormat::Gray ? TVPLayerTextureFormat::R8 : TVPLayerTextureFormat::RGBA8);
        if(!handle) throw std::runtime_error("GPU Layer texture allocation failed");
        session->textures.insert(this); session->stats.gpuResidentBytes+=Bytes();
    }
    ~LayerTexture() override {
        if(handle && session->backend) session->backend->DestroyLayerTexture(handle);
        if(handle) session->stats.gpuResidentBytes-=Bytes();
        if(valid) session->stats.cpuCacheBytes-=Bytes();
        if(pinned) --session->stats.pinnedCPUTextures;
        session->textures.erase(this);
    }
    void Detach() {
        try { Read(); }
        catch(const std::exception& error) {
            TVPConsoleLog("GPU Layer shutdown readback failed: %s",error.what());
            if(pixels.size()!=Bytes()) pixels.assign(Bytes(),0);
            if(!valid) { valid=true; session->stats.cpuCacheBytes+=Bytes(); }
        }
        session->backend->DestroyLayerTexture(handle); handle=nullptr;
        session->stats.gpuResidentBytes-=Bytes();
        if(!pinned) { pinned=true; ++session->stats.pinnedCPUTextures; }
    }
    bool Belongs(const std::shared_ptr<Session>& s) const { return session==s && handle; }
    TVPTextureFormat::e GetFormat() const override { return format; }
    tjs_int GetPitch() const override { return Width*(format==TVPTextureFormat::Gray ? 1 : 4); }
    bool IsCPUResident() const override { return pinned || !handle; }
    bool IsStatic() override { return readonly && !pinned; }
    bool IsOpaque() override { return false; }
    const void* GetScanLineForRead(tjs_uint y) override { Read(); return pixels.data()+size_t(y)*GetPitch(); }
    void* GetScanLineForWrite(tjs_uint y) override {
        Read();
        // Callers commonly stride past the requested row, so this cannot be
        // narrowed to a single row without a contract they do not follow.
        if(handle) MarkDirtyAll();
        return pixels.data()+size_t(y)*GetPitch();
    }
    void* LockCPURead() override { Read(); ++locks; return pixels.data(); }
    void UnlockCPU() override { if(locks) --locks; }
    void MarkCPUModified() override { Read(); MarkDirtyAll(); }
    void MarkCPUModified(const tTVPRect& written) override { Read(); MarkDirty(written); }
    void InvalidateCPUCache() override {
        // The GPU now owns these pixels, so no pre-lease CPU damage survives.
        dirty=false; leaseHadDamage=false;
        if(valid) { valid=false; session->stats.cpuCacheBytes-=Bytes(); }
        // A stale read pointer is only valid through its read lock. Retaining
        // allocation prevents accidental relocation during an active lock.
        if(!locks && !pinned) std::vector<uint8_t>().swap(pixels);
    }
    void* GetPersistentCPUData(bool write) override {
        Read(); if(!pinned) { pinned=true; ++session->stats.pinnedCPUTextures; }
        if(write) {
            if(!writeLeased) { leaseHadDamage=dirty; leaseDamage=damage; writeLeased=true; }
            MarkDirtyAll();
        }
        return pixels.data();
    }
    // Ends a write lease opened by GetPersistentCPUData(true). Writers that can
    // describe what they touched report it here and stop paying for full
    // re-uploads; the pointer stays valid because the texture remains pinned.
    void ReleasePersistentCPUData(const tTVPRect* written) override {
        if(!writeLeased) return;
        writeLeased=false;
        if(written) {
            // Re-narrow to what the lease actually wrote, plus anything that was
            // already pending when it opened.
            dirty=leaseHadDamage; damage=leaseDamage; MarkDirty(*written);
        } else MarkDirtyAll();
        leaseHadDamage=false;
    }
    void* GetTextureHandle() override {
        if(!session->backend || !handle) return nullptr;
        // Only actual CPU writes need an upload. A pinned texture keeps its raw
        // pointer alive but is not itself a reason to re-send unchanged pixels.
        if(dirty || writeLeased) {
            if(writeLeased) MarkDirtyAll();
            Read();
            const int bpp=format==TVPTextureFormat::Gray ? 1 : 4;
            const auto* source=pixels.data()+size_t(damage.top)*GetPitch()+size_t(damage.left)*bpp;
            if(!session->backend->UpdateLayerTexture(handle,source,GetPitch(),Rect(damage)))
                throw std::runtime_error("GPU Layer upload failed");
            session->stats.uploadedBytes+=size_t(damage.get_width())*bpp*damage.get_height();
            dirty=false; leaseHadDamage=false;
        }
        return handle;
    }
    void Update(const void* data,TVPTextureFormat::e f,int pitch,const tTVPRect& requested) override {
        // Video/AlphaMovie frames may extend beyond the canvas. Empty and fully
        // clipped updates are no-ops; the source describes the requested ROI.
        if(requested.get_width()<=0 || requested.get_height()<=0) return;
        tTVPRect r(std::max(0,requested.left),std::max(0,requested.top),
                   std::min(Width,requested.right),std::min(Height,requested.bottom));
        if(r.get_width()<=0 || r.get_height()<=0) return;
        int bpp=format==TVPTextureFormat::Gray ? 1 : 4;
        if(!data || f!=format || pitch<=0 || size_t(pitch)<size_t(requested.get_width())*bpp) {
            throw std::runtime_error("Invalid GPU Layer update: format="+std::to_string(int(f))+
                " targetFormat="+std::to_string(int(format))+" pitch="+std::to_string(pitch)+
                " texture="+std::to_string(Width)+"x"+std::to_string(Height)+
                " rect="+std::to_string(requested.left)+","+std::to_string(requested.top)+","+
                std::to_string(requested.right)+","+std::to_string(requested.bottom));
        }
        const auto* source=static_cast<const uint8_t*>(data)+size_t(r.top-requested.top)*pitch+
                           size_t(r.left-requested.left)*bpp;
        int bytes=r.get_width()*bpp;
        if(pinned || dirty || !handle) {
            Read(); for(int y=0;y<r.get_height();++y)
                std::memcpy(pixels.data()+size_t(y+r.top)*GetPitch()+r.left*bpp,source+size_t(y)*pitch,bytes);
            MarkDirty(r); return;
        }
        if(!session->backend->UpdateLayerTexture(handle,source,pitch,Rect(r)))
            throw std::runtime_error("GPU Layer update failed");
        session->stats.uploadedBytes+=size_t(bytes)*r.get_height(); InvalidateCPUCache();
    }
    uint32_t GetPoint(int x,int y) override {
        if(x<0 || y<0 || x>=Width || y>=Height) return 0;
        auto* p=static_cast<const uint8_t*>(GetScanLineForRead(y));
        if(format==TVPTextureFormat::Gray) return p[x];
        uint32_t v; std::memcpy(&v,p+x*4,4); return v;
    }
    void SetPoint(int x,int y,uint32_t color) override {
        if(x<0 || y<0 || x>=Width || y>=Height) return;
        tTVPRect r(x,y,x+1,y+1); Update(&color,format,format==TVPTextureFormat::Gray?1:4,r);
    }
    bool GetTextureData(void* destination,tjs_int& pitch) override {
        pitch=GetPitch(); if(destination) *static_cast<void**>(destination)=GetPersistentCPUData(false); return false;
    }
};

// Borrowed software views exist only during a fallback call. The software
// manager owns neither their pixels nor their lifetime; aliasing is preserved.
class CPUViews {
    std::unordered_map<iTVPTexture2D*,std::unique_ptr<iTVPTexture2D>> views;
public:
    iTVPTexture2D* Get(iTVPTexture2D* t) {
        if(!t || !dynamic_cast<LayerTexture*>(t)) return t;
        auto& v=views[t];
        if(!v) v.reset(TVPGetSoftwareRenderManager()->CreateTexture2D(t->GetScanLineForRead(0),t->GetPitch(),t->GetWidth(),t->GetHeight(),t->GetFormat()));
        return v.get();
    }
};

class LayerManager final : public iTVPRenderManager {
public:
    std::shared_ptr<Session> session;
    int stretch=0;
    iTVPRenderManager* Software() { return TVPGetSoftwareRenderManager(); }
    const char* GetName() override { return "Metal Layer"; }
    // This facade keeps the software ABI, including CPU plugin operations.
    bool IsSoftware() override { return false; }
    iTVPRenderMethod* GetRenderMethod(const char* name,uint32_t* hint=nullptr) override { return Software()->GetRenderMethod(name,hint); }
    int EnumParameterID(const char* name) override { return Software()->EnumParameterID(name); }
    void SetParameterInt(int id,int value) override {
        Software()->SetParameterInt(id,value);
        if(id==Software()->EnumParameterID("StretchType")) stretch=value;
    }
    iTVPTexture2D* CreateTexture2D(const void* data,int pitch,unsigned w,unsigned h,TVPTextureFormat::e format,int flags=0) override {
        if(!session || (format!=TVPTextureFormat::RGBA && format!=TVPTextureFormat::Gray))
            return Software()->CreateTexture2D(data,pitch,w,h,format,flags);
        std::unique_ptr<LayerTexture> t(new LayerTexture(session,w,h,format,data || (flags&RENDER_CREATE_TEXTURE_FLAG_STATIC)));
        if(data) t->Update(data,format,pitch,tTVPRect(0,0,w,h));
        return t.release();
    }
    iTVPTexture2D* CreateTexture2D(tTVPBitmap* bmp) override {
        return CreateTexture2D(bmp->GetBits(),bmp->GetPitch(),bmp->GetWidth(),bmp->GetHeight(),bmp->GetBPP()==8?TVPTextureFormat::Gray:TVPTextureFormat::RGBA,RENDER_CREATE_TEXTURE_FLAG_STATIC);
    }
    iTVPTexture2D* CreateTexture2D(TJS::tTJSBinaryStream* stream) override { return Software()->CreateTexture2D(stream); }
    iTVPTexture2D* CreateTexture2D(unsigned w,unsigned h,iTVPTexture2D* old) override {
        auto* t=CreateTexture2D(nullptr,0,w,h,old->GetFormat());
        tTVPRect r(0,0,std::min(w,old->GetWidth()),std::min(h,old->GetHeight()));
        std::pair<iTVPTexture2D*,tTVPRect> pair(old,r);
        OperateRect(GetRenderMethod("Copy"),t,nullptr,r,tRenderTexRectArray(&pair,1)); return t;
    }
    bool GetRenderStat(unsigned& count,uint64_t& memory) override {
        count=session?static_cast<unsigned>(session->stats.gpuOperations+session->stats.cpuFallbacks):0;
        memory=session?session->stats.gpuResidentBytes:0; return true;
    }
    bool GetTextureStat(iTVPTexture2D* t,uint64_t& memory) override {
        memory=t?uint64_t(t->GetPitch())*t->GetHeight():0; return t!=nullptr;
    }
    bool GPU(iTVPRenderMethod* method,iTVPTexture2D* target,iTVPTexture2D* reference,tTVPRect dst,const tRenderTexRectArray& inputs) {
        auto* t=dynamic_cast<LayerTexture*>(target); TVPLayerOperation op;
        if(!session || !t || !t->Belongs(session) || t->IsCPUResident() || inputs.size()>1 ||
            !method->DescribeGpuOperation(op) || stretch<0 || stretch>2) return false;
        if(op.opacity<0 || op.opacity>255) return false;
        LayerTexture* source=nullptr; tTVPRect src(0,0,1,1);
        if(inputs.size()) {
            source=dynamic_cast<LayerTexture*>(inputs[0].first); src=inputs[0].second;
            if(!source || !source->Belongs(session)) return false;
            if(op.kind==TVPLayerOperationKind::ColorMap) { if(source->GetFormat()!=TVPTextureFormat::Gray) return false; }
            else if(source->GetFormat()!=TVPTextureFormat::RGBA) return false;
            int sw=src.get_width(),sh=src.get_height(),dw=dst.get_width(),dh=dst.get_height();
            if(sw==0 || sh==0 || dw<=0 || dh<=0) return false;
            if(sw>0 && sh>0 && (sw!=dw || sh!=dh)) {
                // Match the software manager's integer source adjustments before resize.
                if(dst.left<0) { src.left+=float(sw)/dw*-dst.left; dst.left=0; }
                if(dst.right>int(t->GetWidth())) { src.right-=float(src.get_width())/dst.get_width()*(dst.right-t->GetWidth()); dst.right=t->GetWidth(); }
                if(dst.top<0) { src.top+=float(sh)/dh*-dst.top; dst.top=0; }
                if(dst.bottom>int(t->GetHeight())) { src.bottom-=float(src.get_height())/dst.get_height()*(dst.bottom-t->GetHeight()); dst.bottom=t->GetHeight(); }
                if(src.get_width()==0 || src.get_height()==0 || dst.get_width()<=0 || dst.get_height()<=0) return true;
            } else if((sw<0 || sh<0) && (std::abs(sw)!=dw || std::abs(sh)!=dh)) return false;
        } else if(op.kind!=TVPLayerOperationKind::Fill && op.kind!=TVPLayerOperationKind::FillColor && op.kind!=TVPLayerOperationKind::FillMask && op.kind!=TVPLayerOperationKind::FillBlend) return false;
        if(!session->tablesReady) {
            if(!session->backend->SetLayerAlphaTables(TVPOpacityOnOpacityTable,TVPNegativeMulTable)) return false;
            session->tablesReady=true;
        }
        void* sh=source?source->GetTextureHandle():nullptr;
        if(!session->backend->OperateLayerRect(op,t->GetTextureHandle(),Rect(dst),sh,Rect(src),stretch==0?0:1)) return false;
        t->InvalidateCPUCache(); ++session->stats.gpuOperations; return true;
    }
    void OperateRect(iTVPRenderMethod* method,iTVPTexture2D* target,iTVPTexture2D* reference,const tTVPRect& dst,const tRenderTexRectArray& inputs) override {
        if(GPU(method,target,reference,dst,inputs)) return;
        CPUViews views; std::vector<std::pair<iTVPTexture2D*,tTVPRect>> textures;
        for(size_t i=0;i<inputs.size();++i) textures.emplace_back(views.Get(inputs[i].first),inputs[i].second);
        Software()->OperateRect(method,views.Get(target),views.Get(reference),dst,tRenderTexRectArray(textures.data(),textures.size()));
        // The software manager clips its writes to dst, so the upload can too.
        if(target) target->MarkCPUModified(dst);
        if(session) ++session->stats.cpuFallbacks;
    }
    void OperateTriangles(iTVPRenderMethod* method,int count,iTVPTexture2D* target,iTVPTexture2D* reference,const tTVPRect& clip,const tTVPPointD* points,const tRenderTexQuadArray& inputs) override {
        CPUViews views; std::vector<std::pair<iTVPTexture2D*,const tTVPPointD*>> textures;
        for(size_t i=0;i<inputs.size();++i) textures.emplace_back(views.Get(inputs[i].first),inputs[i].second);
        Software()->OperateTriangles(method,count,views.Get(target),views.Get(reference),clip,points,tRenderTexQuadArray(textures.data(),textures.size()));
        if(target) target->MarkCPUModified(); if(session) ++session->stats.cpuFallbacks;
    }
    void OperatePerspective(iTVPRenderMethod* method,int count,iTVPTexture2D* target,iTVPTexture2D* reference,const tTVPRect& clip,const tTVPPointD* points,const tRenderTexQuadArray& inputs) override {
        CPUViews views; std::vector<std::pair<iTVPTexture2D*,const tTVPPointD*>> textures;
        for(size_t i=0;i<inputs.size();++i) textures.emplace_back(views.Get(inputs[i].first),inputs[i].second);
        Software()->OperatePerspective(method,count,views.Get(target),views.Get(reference),clip,points,tRenderTexQuadArray(textures.data(),textures.size()));
        if(target) target->MarkCPUModified(); if(session) ++session->stats.cpuFallbacks;
    }
};
LayerManager& Manager() { static auto* manager=new LayerManager; return *manager; }
}
bool TVPBindMetalLayerRenderManager(krkrsdl3::iTVPRenderBackend* backend) {
    TVPUnbindMetalLayerRenderManager();
    fallbackReason.clear();
    if(!backend || !backend->SupportsLayerOperations()) {
        fallbackReason="ordinary Layer pipeline unavailable"; return false;
    }
    try {
        // Probe both required resource types before binding/creating any Layer.
        for(auto format:{TVPLayerTextureFormat::RGBA8,TVPLayerTextureFormat::R8}) {
            void* probe=backend->CreateLayerTexture(1,1,format);
            if(!probe) { fallbackReason="RGBA/R8 Layer resource initialization failed"; return false; }
            backend->DestroyLayerTexture(probe);
        }
        auto& manager=Manager();
        manager.RenderMethodCache=TVPGetRenderManager(ttstr("software"))->RenderMethodCache;
        manager.session=std::make_shared<Session>(backend); manager.stretch=0;
        TVPSetRenderManager(&manager); return true;
    } catch(const std::exception& error) {
        fallbackReason=error.what(); return false;
    }
}
void TVPUnbindMetalLayerRenderManager() {
    auto& manager=Manager(); TVPSetRenderManager(nullptr);
    if(!manager.session) return;
    // Normally empty after RecycleProcess; survivors become safe software
    // textures instead of retaining a dead backend pointer across sessions.
    for(auto* texture:manager.session->textures) texture->Detach();
    manager.session->backend=nullptr; manager.session.reset();
}
bool TVPMetalLayerCompositionActive() { return bool(Manager().session); }
TVPLayerRenderStats TVPGetMetalLayerRenderStats() { return Manager().session?Manager().session->stats:TVPLayerRenderStats{}; }

const char* TVPMetalLayerFallbackReason() { return fallbackReason.c_str(); }
