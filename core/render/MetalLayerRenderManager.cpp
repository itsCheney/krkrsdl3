#include "tjsCommHead.h"
#include "RenderManager.h"
#include "MetalLayerRenderManager.h"
#include "LayerTransitionGeometry.h"
#include "LayerShrinkGeometry.h"
#include "LayerBitmap.h"
#include "TVPCompositor.h"
#include "TVPMsg.h"
#include "gl/tvpgl.h"
#include "Platform.h"
#include "PointReadTrace.h"
#include "CPUConsumerTrace.h"
#include "LayerTriangleTrace.h"
#include "LayerAffineGeometry.h"
#include "LayerPerspectiveGeometry.h"
#include "AsyncAlphaTileCache.h"
#include "../../plugins/emoteplayer/emoteperformance.h"
#include "tjsDebug.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <SDL3/SDL.h>

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[65536];
extern unsigned char TVPNegativeMulTable[65536];
}

namespace {
using krkrsdl3::iTVPRenderBackend;
namespace point_trace = krkrsdl3::point_trace;
namespace triangle_trace = krkrsdl3::layer_triangle_trace;
using TriangleClock = std::chrono::steady_clock;
uint64_t ElapsedNS(TriangleClock::time_point start,TriangleClock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
}
// At most 32 distinct short keys per interval; summaries show the top eight.
// Overflow is billed to otherCalls rather than dropping calls or growing maps.
struct TriangleHistogram {
    std::unordered_map<std::string,uint64_t> counts;
    uint64_t otherCalls = 0;
    void Add(std::string key) {
        if(key.size()>40) {
            size_t cut=39;
            while(cut && (static_cast<unsigned char>(key[cut])&0xc0)==0x80) --cut;
            key.resize(cut); key+='~';
        }
        auto it=counts.find(key);
        if(it!=counts.end()) ++it->second;
        else if(counts.size()<32) counts.emplace(std::move(key),1);
        else ++otherCalls;
    }
    std::string Summary() const {
        std::vector<std::pair<std::string,uint64_t>> ordered(counts.begin(),counts.end());
        std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) {
            return a.second!=b.second ? a.second>b.second : a.first<b.first;
        });
        std::string out;
        uint64_t omitted=otherCalls;
        for(size_t i=0;i<ordered.size();++i) {
            if(i>=8) { omitted+=ordered[i].second; continue; }
            if(!out.empty()) out+=',';
            out+=ordered[i].first+":"+std::to_string(ordered[i].second);
        }
        if(omitted) {
            if(!out.empty()) out+=',';
            out+="otherCalls:"+std::to_string(omitted);
        }
        return out;
    }
};
struct TriangleInterval {
    TriangleClock::time_point started = TriangleClock::now();
    TVPLayerTriangleFallbackStats stats;
    TriangleHistogram methods, targetSizes, stretchModes;
    uint64_t sources[static_cast<int>(triangle_trace::Source::Count)] = {};
};
class LayerTexture;
bool CaptureCPUProducer(void* texture,krkrsdl3::cpu_consumer_trace::Producer& result);
void LogCPUConsumer(const char* message) {TVPConsoleLog("%s",message);}
std::string fallbackReason;
struct Session {
    const uint64_t diagnosticID=krkrsdl3::cpu_consumer_trace::nextSession.fetch_add(1,std::memory_order_relaxed);
    iTVPRenderBackend* backend;
    TVPLayerRenderStats stats;
    std::set<LayerTexture*> textures;
    std::unordered_map<std::string,uint64_t> multipleInputMethods;
    std::unordered_map<std::string,uint64_t> unsupportedMethods;
    TriangleInterval triangles;
    bool tablesReady = false;
    bool psTablesReady = false;
    TVPLayerParameterUploadStats parameterBaseline;
    unsigned sourceRejectionReports=0;
    explicit Session(iTVPRenderBackend* b) : backend(b), parameterBaseline(b->GetLayerParameterUploadStats()) {
        krkrsdl3::cpu_consumer_trace::SetCallbacks(LogCPUConsumer,CaptureCPUProducer);
    }
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
    unsigned scopedWrites = 0;
    char uploadOrigin[48]="initial", rawWriteOrigin[48]="script.rawWrite";
    bool uploadOriginOversize=false, rawWriteOriginOversize=false;
    // Union of CPU writes since the last upload; meaningful only while dirty.
    tTVPRect damage;
    // Damage outstanding when the current write lease opened. A lease widens
    // damage to the whole surface while it is held, so releasing it must restore
    // this rather than drop writes the lease did not make.
    bool leaseHadDamage = false;
    tTVPRect leaseDamage;
    const uint64_t textureID = point_trace::NextTextureID();
    uint64_t contentVersion = 0;
    bool emoteAlphaEligible = false;
    krkrsdl3::AsyncAlphaTileCache alphaTiles;
    struct InvalidationInfo {
        point_trace::Invalidation reason = point_trace::Invalidation::Unknown;
        const char* writer = "initial";
        uint64_t version = 0;
    };
    InvalidationInfo lastWrite;
    tTVPRect lastWriteRect{0,0,0,0};
    struct PointCacheEntry {
        int x = -1, y = -1;
        uint32_t value = 0;
        bool valid = false;
        bool alphaValid = false;
        InvalidationInfo colorInvalidation, alphaInvalidation;
    };
    static constexpr size_t kPointCacheSize = 32;
    PointCacheEntry pointCache[kPointCacheSize];
    size_t pointCacheNext = 0;
    void InvalidatePointCache(const tTVPRect* written=nullptr,bool preserveAlpha=false,
                             point_trace::Invalidation reason=point_trace::Invalidation::Explicit,
                             const char* writer="external") {
        lastWrite = {reason, point_trace::CurrentWriter() ? point_trace::CurrentWriter() : writer,
                     ++contentVersion};
        lastWriteRect = written ? *written : tTVPRect(0,0,Width,Height);
        for(auto& entry:pointCache) {
            if(written && (entry.x<written->left || entry.x>=written->right ||
                           entry.y<written->top || entry.y>=written->bottom)) continue;
            // Preserve the FIRST write that invalidated this sample. Later
            // unrelated writes are reported separately as lastWrite, not blamed
            // for the cache miss that they did not cause.
            if(entry.valid) entry.colorInvalidation=lastWrite;
            entry.valid=false;
            if(!preserveAlpha) {
                if(entry.alphaValid) entry.alphaInvalidation=lastWrite;
                entry.alphaValid=false;
            }
        }
        if(!written && !preserveAlpha) pointCacheNext=0;
    }
    bool FindPointCache(int x,int y,uint32_t& value,bool alphaOnly,point_trace::Query& trace) {
        for(const auto& entry:pointCache) {
            if((alphaOnly ? entry.alphaValid : entry.valid) && entry.x==x && entry.y==y) {
                value=entry.value;
                ++session->stats.pointCacheHits;
                return true;
            }
            if(entry.x==x && entry.y==y) {
                const auto& invalidation=alphaOnly ? entry.alphaInvalidation : entry.colorInvalidation;
                trace.missReason="invalidated";
                trace.invalidation=invalidation.reason;
                trace.writer=invalidation.writer;
                trace.invalidatedVersion=invalidation.version;
            }
        }
        ++session->stats.pointCacheMisses;
        return false;
    }
    void ReportPointRead(const point_trace::Query& trace) {
        if(!trace.reported || !point_trace::Enabled()) return;
        try {
            TVPConsoleLog("metal.pointRead pointQueryID=%llu source=%s trigger=%s parentTrigger=%s "
                          "owner=%llx textureID=%llu version=%llu x=%d y=%d width=%d height=%d alphaOnly=%d "
                          "miss=%s invalidation=%s writer=%s invalidatedVersion=%llu "
                          "lastWrite=%s lastWriter=%s lastRect=%d,%d,%d,%d "
                          "lastSubmittedID=%llu renderFrame=%llu wallMS=%.3f gpuSyncWaitMS=%.3f",
                          static_cast<unsigned long long>(trace.queryID), point_trace::Name(trace.origin.source),
                          point_trace::Name(trace.origin.trigger), point_trace::Name(trace.origin.parentTrigger),
                          static_cast<unsigned long long>(trace.origin.owner),
                          static_cast<unsigned long long>(trace.textureID), static_cast<unsigned long long>(trace.version),
                          trace.x,trace.y,trace.width,trace.height,trace.alphaOnly?1:0,trace.missReason,
                          point_trace::Name(trace.invalidation),trace.writer,
                          static_cast<unsigned long long>(trace.invalidatedVersion),
                          point_trace::Name(trace.lastInvalidation),trace.lastWriter,
                          trace.lastWriteLeft,trace.lastWriteTop,trace.lastWriteRight,trace.lastWriteBottom,
                          static_cast<unsigned long long>(trace.lastSubmittedID),
                          static_cast<unsigned long long>(trace.renderFrame),
                          double(trace.wallNS)/1000000.0,double(trace.gpuWaitNS)/1000000.0);
            // Capture synchronously while the initiating TJS frames are alive.
            // The host owns tracer lifetime at safe VM boundaries. Bytecode can
            // omit source maps, so even a nonempty trace is not an exact-line guarantee.
            std::string stack=TJSGetStackTraceString(4,TJS_N(" | ")).AsStdString();
            const bool available=!stack.empty();
            if(stack.size()>512) {
                size_t cut=512;
                while(cut && (static_cast<unsigned char>(stack[cut])&0xc0)==0x80) --cut;
                stack.resize(cut);
            }
            for(char& c:stack) if(static_cast<unsigned char>(c)<32 || c=='"') c=' ';
            TVPConsoleLog("metal.pointCaller pointQueryID=%llu traceState=%s positions=unverified trace=\"%s\"",
                          static_cast<unsigned long long>(trace.queryID),available?"captured":"unavailable",stack.c_str());
        } catch(...) {
            // Diagnostic allocation or unavailable VM trace must not affect reads.
        }
    }
    void StorePointCache(int x,int y,uint32_t value) {
        // Refresh alpha-only entries in place, avoiding duplicate coordinates
        // whose old alpha could otherwise survive a later RGB query.
        for(auto& entry:pointCache) {
            if(entry.x==x && entry.y==y) {
                entry.value=value; entry.valid=entry.alphaValid=true; return;
            }
        }
        auto& entry=pointCache[pointCacheNext++ % kPointCacheSize];
        entry.x=x; entry.y=y; entry.value=value; entry.valid=entry.alphaValid=true;
    }
    size_t Bytes() const { return size_t(GetPitch())*Height; }
    // Writers report what they touched so an upload carries only those rows.
    // Callers that cannot describe their writes report the whole surface.
    void MarkDirty(const tTVPRect& requested) {
        tTVPRect r(std::max(0,requested.left),std::max(0,requested.top),
                   std::min(int(Width),requested.right),std::min(int(Height),requested.bottom));
        if(r.get_width()<=0 || r.get_height()<=0) return;
        const char* caller=krkrsdl3::layer_work::source;
        if(!std::strcmp(caller,"unattributed")) caller="bitmap.cpuWrite";
        const bool oversize=std::strlen(caller)>=sizeof(uploadOrigin) || (dirty && uploadOriginOversize);
        if(dirty && std::strcmp(uploadOrigin,caller)) caller="mixed";
        uploadOriginOversize=oversize;
        std::snprintf(uploadOrigin,sizeof(uploadOrigin),"%s",caller);
        InvalidatePointCache(&r,false,point_trace::Invalidation::CPUWrite,"cpu.write");
        if(!dirty) { damage=r; dirty=true; return; }
        damage.left=std::min(damage.left,r.left); damage.top=std::min(damage.top,r.top);
        damage.right=std::max(damage.right,r.right); damage.bottom=std::max(damage.bottom,r.bottom);
    }
    void MarkDirtyAll() { MarkDirty(tTVPRect(0,0,Width,Height)); }
    bool Read(TVPLayerReadbackSource source) {
        if(valid) return false;
        if(!session->backend || !handle) throw std::runtime_error("GPU Layer read without a session");
        int pitch=0;
        const auto diagnosticEpoch=krkrsdl3::layer_work::CaptureGeneration();
        const bool diagnostics=diagnosticEpoch!=0;
        auto consumer=krkrsdl3::cpu_consumer_trace::BeginRead(diagnosticEpoch,session->diagnosticID);
        if(consumer.epoch) {
            consumer.textureID=textureID;consumer.contentVersion=contentVersion;
            consumer.width=Width;consumer.height=Height;consumer.lastWriter=lastWrite.writer;
            consumer.lastWrite[0]=lastWriteRect.left;consumer.lastWrite[1]=lastWriteRect.top;
            consumer.lastWrite[2]=lastWriteRect.right;consumer.lastWrite[3]=lastWriteRect.bottom;
        }
        const auto started=diagnostics ? krkrsdl3::layer_work::Now() : 0;
        if(!session->backend->ReadLayerTexture(handle,pixels,pitch) || pitch!=GetPitch())
            throw std::runtime_error("GPU Layer readback failed");
        constexpr const char* categories[]={"bitmap.lock","fallback","script.rawPointer","bitmap.scanline","session.detach","bitmap.point"};
        const char* origin=krkrsdl3::layer_work::source;
        if(!std::strcmp(origin,"unattributed")) origin=categories[static_cast<int>(source)];
        if(diagnostics) {
            const auto wallNS=krkrsdl3::layer_work::Now()-started;
            const auto waitNS=session->backend->GetLastReadbackWaitNanoseconds();
            krkrsdl3::layer_work::Record(false,textureID,Width,Height,Bytes(),wallNS,waitNS,false,origin,diagnosticEpoch);
            consumer.source=origin;consumer.bytes=Bytes();consumer.wallNS=wallNS;consumer.waitNS=waitNS;
            if(krkrsdl3::cpu_consumer_trace::ReportRead(consumer)) {
                try {
                    const auto stack=TJSGetStackTraceString(4,TJS_N(" | ")).AsStdString();
                    krkrsdl3::cpu_consumer_trace::ReportCaller(consumer,stack.c_str());
                } catch(...) {krkrsdl3::cpu_consumer_trace::ReportCaller(consumer,nullptr);}
            }
        }
        session->stats.readbackBytes+=Bytes(); session->stats.cpuCacheBytes+=Bytes(); valid=true;
        const int index=static_cast<int>(source);
        session->stats.readbackBytesBySource[index]+=Bytes();
        ++session->stats.readbackCountBySource[index];
        return true;
    }
public:
    uint64_t DiagnosticSessionID() const {return session->diagnosticID;}
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
        alphaTiles.Cancel();
        try { Read(TVPLayerReadbackSource::Detach); }
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
    void MarkEmoteAlpha() { emoteAlphaEligible=true; }
    bool HasEmoteAlpha() const { return format==TVPTextureFormat::RGBA && handle && session->backend && !pinned && !writeLeased && !scopedWrites; }
    std::shared_ptr<krkrsdl3::AsyncAlphaTile> DemandAlpha(int x,int y) {
        return alphaTiles.Demand(x,y,Width,Height);
    }
    void StopAlphaDemand() { alphaTiles.StopDemand(); }
    void EncodeAlphaForPresentation(const std::shared_ptr<krkrsdl3::AsyncLayerPresentation>& frozen={}) {
        if(!HasEmoteAlpha() || !emoteplayer::performanceEnabled("MIKAGE_EMOTE_ASYNC_ALPHA")) return;
        // Upload CPU damage before recording the content key, then read exactly
        // this MainImage version after the window's normal composition pass.
        GetTextureHandle();
        alphaTiles.EncodeDemanded(contentVersion,frozen ? frozen : session->backend->GetCurrentLayerPresentation(),
            [&](const std::shared_ptr<krkrsdl3::AsyncLayerReadback>& read) {
                const bool encoded=session->backend->RequestLayerTextureRegionRead(handle,read->region,read);
                if(encoded) {
                    auto& counters=emoteplayer::performanceCounters();
                    counters.alphaRequests.fetch_add(1,std::memory_order_relaxed);
                    counters.alphaReadBytes.fetch_add(uint64_t(read->region.Width())*read->region.Height()*4,
                                                     std::memory_order_relaxed);
                }
                return encoded;
            });
    }
    // Used when building a borrowed software view for a CPU fallback, so the
    // readback is billed to the fallback rather than to ordinary pixel access.
    const void* ScanLineForFallback(TVPLayerFallbackReadbackRole role) {
        if(Read(TVPLayerReadbackSource::Fallback)) {
            const int index=static_cast<int>(role);
            session->stats.fallbackReadbackBytesByRole[index]+=Bytes();
            ++session->stats.fallbackReadbackCountByRole[index];
        }
        return pixels.data();
    }
    TVPTextureFormat::e GetFormat() const override { return format; }
    tjs_int GetPitch() const override { return Width*(format==TVPTextureFormat::Gray ? 1 : 4); }
    bool IsCPUResident() const override { return pinned || !handle || scopedWrites; }
    bool HasCPUAccess() const { return pinned || locks || writeLeased || scopedWrites; }
    bool IsStatic() override { return readonly && !pinned; }
    bool IsOpaque() override { return false; }
    const void* GetScanLineForRead(tjs_uint y) override { Read(TVPLayerReadbackSource::Pixels); return pixels.data()+size_t(y)*GetPitch(); }
    void* GetScanLineForWrite(tjs_uint y) override {
        Read(TVPLayerReadbackSource::Pixels);
        // Callers commonly stride past the requested row, so this cannot be
        // narrowed to a single row without a contract they do not follow.
        if(handle) MarkDirtyAll();
        return pixels.data()+size_t(y)*GetPitch();
    }
    void* LockCPURead() override { Read(TVPLayerReadbackSource::Lock); ++locks; return pixels.data(); }
    void UnlockCPU() override { if(locks) --locks; }
    void* LockCPUWrite() override {
        Read(TVPLayerReadbackSource::Pixels); ++locks; ++scopedWrites; return pixels.data();
    }
    void UnlockCPUWrite(const tTVPRect& written) override {
        if(!scopedWrites) return;
        MarkDirty(written); --scopedWrites; UnlockCPU();
        const auto* consumer=krkrsdl3::cpu_consumer_trace::Current();
        if(krkrsdl3::cpu_consumer_trace::IsShrinkMethod(consumer) && consumer->shrinkSucceeded &&
           written.get_width()>0 && written.get_height()>0)
            krkrsdl3::cpu_consumer_trace::ReportShrinkOutput(this,written.left,written.top,written.right,written.bottom);
    }
    void* LockCPUWriteForOverwrite() override {
        if(pinned || writeLeased || locks) return LockCPUWrite();
        if(!valid) {
            pixels.resize(Bytes()); valid=true; session->stats.cpuCacheBytes+=Bytes();
        }
        ++locks; ++scopedWrites; return pixels.data();
    }
    void MarkCPUModified() override { Read(TVPLayerReadbackSource::Fallback); MarkDirtyAll(); }
    void MarkCPUModified(const tTVPRect& written) override { Read(TVPLayerReadbackSource::Fallback); MarkDirty(written); }
    void InvalidateCPUCache() override {
        InvalidatePointCache();
        DiscardCPUCache();
    }
    void InvalidateCPUCacheRegion(const tTVPRect& written,bool preserveAlpha=false,
                                 point_trace::Invalidation reason=point_trace::Invalidation::GPUOperation,
                                 const char* writer="layer.gpuRect") {
        InvalidatePointCache(&written,preserveAlpha,reason,writer);
        DiscardCPUCache();
    }
    void DiscardCPUCache() {
        // The GPU now owns these pixels, so no pre-lease CPU damage survives.
        dirty=false; leaseHadDamage=false;
        if(valid) { valid=false; session->stats.cpuCacheBytes-=Bytes(); }
        // A stale read pointer is only valid through its read lock. Retaining
        // allocation prevents accidental relocation during an active lock.
        if(!locks && !pinned) std::vector<uint8_t>().swap(pixels);
    }
    void* GetPersistentCPUData(bool write) override {
        Read(TVPLayerReadbackSource::Persistent); if(!pinned) { pinned=true; ++session->stats.pinnedCPUTextures; }
        if(write) {
            rawWriteOriginOversize=std::strlen(krkrsdl3::layer_work::source)>=sizeof(rawWriteOrigin) ||
                (writeLeased && rawWriteOriginOversize);
            std::snprintf(rawWriteOrigin,sizeof(rawWriteOrigin),"%s",
                std::strcmp(krkrsdl3::layer_work::source,"unattributed") ? krkrsdl3::layer_work::source : "script.rawWrite");
            if(!writeLeased) { leaseHadDamage=dirty; leaseDamage=damage; writeLeased=true; }
            MarkDirtyAll();
        }
        return pixels.data();
    }
    // The caller overwrites every pixel, so the GPU's current contents are
    // irrelevant: allocate the cache without a readback rather than fetching
    // pixels that are about to be discarded.
    void* GetPersistentCPUDataForOverwrite() override {
        if(!valid) {
            if(pixels.size()!=Bytes()) pixels.resize(Bytes());
            valid=true; session->stats.cpuCacheBytes+=Bytes();
        }
        if(!pinned) { pinned=true; ++session->stats.pinnedCPUTextures; }
        if(!writeLeased) { leaseHadDamage=dirty; leaseDamage=damage; writeLeased=true; }
        MarkDirtyAll();
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
        if(dirty || writeLeased || scopedWrites) {
            if(writeLeased || scopedWrites) MarkDirtyAll();
            Read(TVPLayerReadbackSource::Pixels);
            const int bpp=format==TVPTextureFormat::Gray ? 1 : 4;
            const auto* source=pixels.data()+size_t(damage.top)*GetPitch()+size_t(damage.left)*bpp;
            const auto diagnosticEpoch=krkrsdl3::layer_work::CaptureGeneration();
            const auto started=diagnosticEpoch ? krkrsdl3::layer_work::Now() : 0;
            if(!session->backend->UpdateLayerTexture(handle,source,GetPitch(),Rect(damage)))
                throw std::runtime_error("GPU Layer upload failed");
            // A caller may query then modify an outstanding CPU write pointer.
            // Samples taken while dirty cannot outlive uploading that damage.
            InvalidatePointCache(&damage,false,point_trace::Invalidation::CPUUpload,"cpu.upload");
            session->stats.uploadedBytes+=size_t(damage.get_width())*bpp*damage.get_height();
            krkrsdl3::layer_work::Record(true,textureID,Width,Height,size_t(damage.get_width())*bpp*damage.get_height(),
                started ? krkrsdl3::layer_work::Now()-started : 0,0,writeLeased || scopedWrites,
                writeLeased ? rawWriteOrigin : uploadOrigin,diagnosticEpoch,
                writeLeased ? rawWriteOriginOversize : uploadOriginOversize);
            dirty=false; leaseHadDamage=false;
        }
        return handle;
    }
    bool GetContentKey(uint64_t& identity,uint64_t& version) const override {
        identity=textureID; version=contentVersion; return true;
    }
    void* GetTextureHandleForRegionWrite() override {
        if(!session->backend || !handle || pinned || locks || writeLeased) return nullptr;
        return GetTextureHandle();
    }
    void CommitGPURegionWrite(const tTVPRect& written) override {
        if(pinned || locks || writeLeased) return;
        InvalidateCPUCacheRegion(written,false,point_trace::Invalidation::GPUOverwrite,"gpu.regionOverwrite");
    }
    void* GetTextureHandleForOverwrite() override {
        // A raw CPU address or active read/write lease can be observed outside
        // this call; replacing GPU contents behind it would violate that contract.
        if(!session->backend || !handle || pinned || locks || writeLeased) return nullptr;
        return handle;
    }
    void CommitGPUOverwrite() override {
        // Called only after a successful full-surface GPU copy. Old CPU damage
        // and cached pixels are obsolete and must never upload over the copy.
        if(pinned || locks || writeLeased) return;
        InvalidatePointCache(nullptr,false,point_trace::Invalidation::GPUOverwrite,"gpu.overwrite");
        dirty=false; leaseHadDamage=false;
        if(valid) {
            valid=false;
            session->stats.cpuCacheBytes-=Bytes();
        }
        std::vector<uint8_t>().swap(pixels);
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
        if(pinned || dirty || locks || !handle) {
            Read(TVPLayerReadbackSource::Pixels); for(int y=0;y<r.get_height();++y)
                std::memcpy(pixels.data()+size_t(y+r.top)*GetPitch()+r.left*bpp,source+size_t(y)*pitch,bytes);
            MarkDirty(r); return;
        }
        const auto diagnosticEpoch=krkrsdl3::layer_work::CaptureGeneration();
        const auto started=diagnosticEpoch ? krkrsdl3::layer_work::Now() : 0;
        if(!session->backend->UpdateLayerTexture(handle,source,pitch,Rect(r)))
            throw std::runtime_error("GPU Layer update failed");
        session->stats.uploadedBytes+=size_t(bytes)*r.get_height();
        krkrsdl3::layer_work::Record(true,textureID,Width,Height,size_t(bytes)*r.get_height(),
            started ? krkrsdl3::layer_work::Now()-started : 0,0,false,
            std::strcmp(krkrsdl3::layer_work::source,"unattributed") ? krkrsdl3::layer_work::source : "bitmap.update",diagnosticEpoch);
        InvalidateCPUCacheRegion(r,false,point_trace::Invalidation::GPUUpdate,"texture.update");
    }
    uint32_t ReadPoint(int x,int y,bool alphaOnly) {
        if(x<0 || y<0 || x>=Width || y>=Height) return 0;
        const int bpp=format==TVPTextureFormat::Gray ? 1 : 4;
        if(valid) {
            const auto* p=pixels.data()+size_t(y)*GetPitch()+size_t(x)*bpp;
            uint32_t v=0;
            if(format==TVPTextureFormat::Gray) v=*p;
            else std::memcpy(&v,p,4);
            // Keep queried pixels when a later GPU operation discards the full
            // CPU mirror but leaves this pixel (or its alpha) unchanged.
            StorePointCache(x,y,v);
            return v;
        }
        uint32_t cached=0;
        point_trace::Query trace;
        if(FindPointCache(x,y,cached,alphaOnly,trace)) return cached;
        trace.textureID=textureID; trace.version=contentVersion;
        trace.x=x; trace.y=y; trace.width=Width; trace.height=Height; trace.alphaOnly=alphaOnly;
        trace.lastInvalidation=lastWrite.reason; trace.lastWriter=lastWrite.writer;
        trace.lastWriteLeft=lastWriteRect.left; trace.lastWriteTop=lastWriteRect.top;
        trace.lastWriteRight=lastWriteRect.right; trace.lastWriteBottom=lastWriteRect.bottom;
        point_trace::QueryScope queryScope(trace);
        const auto recordUIWait = [&] {
            if(trace.origin.source!=point_trace::Source::LayerHitTest || !session->backend) return;
            switch(trace.origin.trigger) {
                case point_trace::Trigger::PointerMove: case point_trace::Trigger::PointerDown:
                case point_trace::Trigger::PointerUp: case point_trace::Trigger::Click:
                case point_trace::Trigger::DoubleClick: case point_trace::Trigger::Wheel:
                case point_trace::Trigger::InputRecheck: break;
                default: return; // Explicit script/cursor/hint queries are separate.
            }
            const auto wait=session->backend->GetLastReadbackWaitNanoseconds();
            if(wait) {
                auto& counters=emoteplayer::performanceCounters();
                counters.uiSyncReads.fetch_add(1,std::memory_order_relaxed);
                counters.uiSyncWaitNS.fetch_add(wait,std::memory_order_relaxed);
            }
        };
        if(handle && session->backend) {
            const auto diagnosticEpoch=krkrsdl3::layer_work::CaptureGeneration();
            std::vector<uint8_t> sample; int pitch=0;
            const TVPLayerRect region{x,y,x+1,y+1};
            const bool read=session->backend->ReadLayerTextureRegion(handle,region,sample,pitch);
            recordUIWait();
            ReportPointRead(trace);
            trace.reported=false;
            if(read &&
               pitch>=bpp && sample.size()>=size_t(bpp)) {
                session->stats.readbackBytes+=bpp;
                const int index=static_cast<int>(TVPLayerReadbackSource::Point);
                session->stats.readbackBytesBySource[index]+=bpp;
                ++session->stats.readbackCountBySource[index];
                krkrsdl3::layer_work::Record(false,textureID,Width,Height,bpp,trace.wallNS,trace.gpuWaitNS,
                    false,"bitmap.point",diagnosticEpoch);
                uint32_t value=0;
                if(format==TVPTextureFormat::Gray) value=sample[0];
                else std::memcpy(&value,sample.data(),4);
                StorePointCache(x,y,value);
                return value;
            }
        }
        // Preserve correctness for unsupported/failed region readback paths.
        auto* p=static_cast<const uint8_t*>(GetScanLineForRead(y));
        recordUIWait();
        ReportPointRead(trace);
        if(format==TVPTextureFormat::Gray) return p[x];
        uint32_t v; std::memcpy(&v,p+x*4,4); return v;
    }
    uint32_t GetPoint(int x,int y) override { return ReadPoint(x,y,false); }
    uint32_t GetPointAlpha(int x,int y) override { return ReadPoint(x,y,true)>>24; }
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
bool CaptureCPUProducer(void* texture,krkrsdl3::cpu_consumer_trace::Producer& result) {
    auto* t=dynamic_cast<LayerTexture*>(static_cast<iTVPTexture2D*>(texture));
    if(!t || !t->GetContentKey(result.textureID,result.contentVersion)) return false;
    result.sessionID=t->DiagnosticSessionID();result.width=t->GetWidth();result.height=t->GetHeight();
    return true;
}
class CPUViews {
    std::unordered_map<iTVPTexture2D*,std::unique_ptr<iTVPTexture2D>> views;
public:
    iTVPTexture2D* Get(iTVPTexture2D* t, TVPLayerFallbackReadbackRole role) {
        auto* layer=dynamic_cast<LayerTexture*>(t);
        if(!layer) return t;
        auto& v=views[t];
        if(!v) v.reset(TVPGetSoftwareRenderManager()->CreateTexture2D(
            layer->ScanLineForFallback(role),t->GetPitch(),t->GetWidth(),t->GetHeight(),t->GetFormat()));
        return v.get();
    }
};

class LayerManager final : public iTVPRenderManager {
public:
    std::shared_ptr<Session> session;
    int stretch=0;
    iTVPRenderManager* Software() { return TVPGetSoftwareRenderManager(); }
    bool Reject(TVPLayerGPURejectReason reason) {
        if(session) ++session->stats.gpuRejectCountByReason[static_cast<int>(reason)];
        static constexpr const char* names[]={"render.targetUnavailable","render.cpuResident","render.inputs",
            "render.method","render.stretch","render.opacity","render.sourceUnavailable","render.format",
            "render.geometry","render.kind","render.alphaTables","render.backendFailure","render.triangles",
            "render.perspective","render.psTables","render.affineAlias","render.perspectiveAlias"};
        static_assert(sizeof(names)/sizeof(*names)==size_t(TVPLayerGPURejectReason::Count));
        krkrsdl3::layer_work::RecordTransitionResult(false,names[static_cast<int>(reason)],0);
        return false;
    }
    bool RejectSource(iTVPRenderMethod* method,iTVPTexture2D* texture) {
        if(session && krkrsdl3::layer_work::enabled.load(std::memory_order_relaxed) && session->sourceRejectionReports<8) {
            ++session->sourceRejectionReports;
            auto* layer=dynamic_cast<LayerTexture*>(texture);
            uint64_t id=0,version=0;
            if(texture) texture->GetContentKey(id,version);
            SDL_Log("layer.sourceUnavailable method=%.40s sourceType=%s width=%u height=%u cpuResident=%d currentSession=%d texture=%llu version=%llu",
                method ? method->GetName().c_str() : "unnamed",layer ? "metal-layer" : texture ? "other" : "null",
                texture ? texture->GetWidth() : 0,texture ? texture->GetHeight() : 0,
                texture ? int(texture->IsCPUResident()) : 0,layer ? int(layer->Belongs(session)) : 0,
                static_cast<unsigned long long>(id),static_cast<unsigned long long>(version));
        }
        return Reject(TVPLayerGPURejectReason::SourceUnavailable);
    }
    bool RejectMethod(TVPLayerGPURejectReason reason, iTVPRenderMethod* method, size_t inputCount=0) {
        if(session) {
            ++session->stats.gpuRejectCountByReason[static_cast<int>(reason)];
            const std::string name = method && !method->GetName().empty() ? method->GetName() : "<unnamed>";
            if(reason==TVPLayerGPURejectReason::MultipleInputs)
                ++session->multipleInputMethods[name+"["+std::to_string(inputCount)+"]"];
            else if(reason==TVPLayerGPURejectReason::UnsupportedMethod)
                ++session->unsupportedMethods[name];
        }
        return false;
    }
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
    bool CanReuseCachedTexture(iTVPTexture2D* texture) const override {
        auto* cached=dynamic_cast<LayerTexture*>(texture);
        return session && cached && cached->Belongs(session) && !cached->IsCPUResident();
    }
    bool GPU(iTVPRenderMethod* method,iTVPTexture2D* target,iTVPTexture2D* reference,tTVPRect dst,const tRenderTexRectArray& inputs) {
        auto* t=dynamic_cast<LayerTexture*>(target); TVPLayerOperation op;
        if(!session || !t || !t->Belongs(session)) return Reject(TVPLayerGPURejectReason::TargetUnavailable);
        if(t->IsCPUResident()) return Reject(TVPLayerGPURejectReason::TargetCPUResident);
        if(inputs.size()==3) {
            if(!method->DescribeGpuOperation(op) || op.kind!=TVPLayerOperationKind::UnivTrans)
                return RejectMethod(TVPLayerGPURejectReason::MultipleInputs,method,inputs.size());
            auto* source1=dynamic_cast<LayerTexture*>(inputs[0].first);
            auto* source2=dynamic_cast<LayerTexture*>(inputs[1].first);
            auto* rule=dynamic_cast<LayerTexture*>(inputs[2].first);
            if(!source1 || !source2 || !rule || !source1->Belongs(session) ||
               !source2->Belongs(session) || !rule->Belongs(session))
                return Reject(TVPLayerGPURejectReason::SourceUnavailable);
            if(t->GetFormat()!=TVPTextureFormat::RGBA ||
               source1->GetFormat()!=TVPTextureFormat::RGBA || source2->GetFormat()!=TVPTextureFormat::RGBA ||
               rule->GetFormat()!=TVPTextureFormat::Gray)
                return Reject(TVPLayerGPURejectReason::SourceFormat);
            const int w=dst.get_width(),h=dst.get_height();
            if(w<=0 || h<=0) return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            for(size_t i=0;i<3;++i) {
                const auto& r=inputs[i].second;
                if(r.get_width()!=w || r.get_height()!=h || r.left<0 || r.top<0 ||
                   r.right>int(inputs[i].first->GetWidth()) || r.bottom>int(inputs[i].first->GetHeight()))
                    return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            }
            if((op.flags & TVP_LAYER_DEST_ALPHA) && !session->tablesReady) {
                if(!session->backend->SetLayerAlphaTables(TVPOpacityOnOpacityTable,TVPNegativeMulTable))
                    return Reject(TVPLayerGPURejectReason::AlphaTables);
                session->tablesReady=true;
            }
            if(!session->backend->OperateLayerRectTripleSource(
                    op,t->GetTextureHandle(),Rect(dst),
                    source1->GetTextureHandle(),Rect(inputs[0].second),
                    source2->GetTextureHandle(),Rect(inputs[1].second),
                    rule->GetTextureHandle(),Rect(inputs[2].second)))
                return Reject(TVPLayerGPURejectReason::BackendFailure);
            t->InvalidateCPUCacheRegion(dst,false,point_trace::Invalidation::GPUOperation,"layer.gpuUnivTrans");
            ++session->stats.gpuOperations; return true;
        }
        if(inputs.size()>1) {
            if(inputs.size()!=2 || !method->DescribeGpuOperation(op) ||
               op.kind!=TVPLayerOperationKind::ConstAlphaSD)
                return RejectMethod(TVPLayerGPURejectReason::MultipleInputs,method,inputs.size());
            if(op.opacity<0 || op.opacity>255) return Reject(TVPLayerGPURejectReason::InvalidOpacity);
            auto* source1=dynamic_cast<LayerTexture*>(inputs[0].first);
            auto* source2=dynamic_cast<LayerTexture*>(inputs[1].first);
            const tTVPRect src1=inputs[0].second,src2=inputs[1].second;
            if(!source1 || !source2 || !source1->Belongs(session) || !source2->Belongs(session))
                return Reject(TVPLayerGPURejectReason::SourceUnavailable);
            if(source1->GetFormat()!=TVPTextureFormat::RGBA || source2->GetFormat()!=TVPTextureFormat::RGBA)
                return Reject(TVPLayerGPURejectReason::SourceFormat);
            const auto sameRect=[](const tTVPRect& a,const tTVPRect& b) {
                return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
            };
            // Software transitions can read and write the same backing buffer.
            // If an aliased source is offset from the output rect, software
            // semantics are order-dependent; snapshotting would subtly change it.
            // Only accelerate the unambiguous one-pixel-to-one-pixel alias case.
            if((source1==t && !sameRect(src1,dst)) || (source2==t && !sameRect(src2,dst)))
                return RejectMethod(TVPLayerGPURejectReason::MultipleInputs,method,inputs.size());
            const int dw=dst.get_width(),dh=dst.get_height();
            if(dw<=0 || dh<=0 || src1.get_width()!=dw || src1.get_height()!=dh ||
               src2.get_width()!=dw || src2.get_height()!=dh)
                return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            if(!session->tablesReady) {
                if(!session->backend->SetLayerAlphaTables(TVPOpacityOnOpacityTable,TVPNegativeMulTable))
                    return Reject(TVPLayerGPURejectReason::AlphaTables);
                session->tablesReady=true;
            }
            if(!session->backend->OperateLayerRectDualSource(
                    op,t->GetTextureHandle(),Rect(dst),
                    source1->GetTextureHandle(),Rect(src1),
                    source2->GetTextureHandle(),Rect(src2)))
                return Reject(TVPLayerGPURejectReason::BackendFailure);
            t->InvalidateCPUCacheRegion(dst,false,point_trace::Invalidation::GPUOperation,"layer.gpuDualSource");
            ++session->stats.gpuOperations; return true;
        }
        if(!method->DescribeGpuOperation(op)) return RejectMethod(TVPLayerGPURejectReason::UnsupportedMethod,method);
        const auto* traits=TVPGetLayerOperationTraits(op.kind);
        if(!traits) return Reject(TVPLayerGPURejectReason::UnsupportedKind);
        // ApplySelf copies the reference before converting, including after COW.
        if(traits->referenceRule==TVPLayerReferenceRule::UsedWhenNoInput && inputs.size()==0) {
            if(!reference) return Reject(TVPLayerGPURejectReason::SourceUnavailable);
            tRenderTexRectArray::Element input(reference,dst);
            return GPU(method,target,nullptr,dst,tRenderTexRectArray(&input,1));
        }
        if(op.kind==TVPLayerOperationKind::UnivTrans || op.kind==TVPLayerOperationKind::ConstAlphaSD)
            return RejectMethod(TVPLayerGPURejectReason::MultipleInputs,method,inputs.size());
        // Ordinary software rectangles ignore the filter for 1:1 operations,
        // and map cubic/higher positive values to ResizeRGBA's bilinear path.
        // Affine/triangle filters have a separate, stricter contract below.
        if(stretch<0) return Reject(TVPLayerGPURejectReason::UnsupportedStretch);
        if(op.opacity<0 || op.opacity>255) return Reject(TVPLayerGPURejectReason::InvalidOpacity);
        LayerTexture* source=nullptr; tTVPRect src(0,0,1,1);
        if(inputs.size()) {
            source=dynamic_cast<LayerTexture*>(inputs[0].first); src=inputs[0].second;
            if(!source || !source->Belongs(session)) return RejectSource(method,inputs[0].first);
            // Offset self-blends are scanline-order dependent in software.
            // A GPU snapshot would change them; same-pixel aliases are safe.
            if(traits->aliasRule==TVPLayerAliasRule::SamePixelOnly && source==t && (src.left!=dst.left || src.top!=dst.top ||
                src.right!=dst.right || src.bottom!=dst.bottom))
                return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            if(traits->sourceFormats[0]==TVPLayerTextureFormat::R8) {
                if(source->GetFormat()!=TVPTextureFormat::Gray) return Reject(TVPLayerGPURejectReason::SourceFormat);
            } else if(source->GetFormat()!=TVPTextureFormat::RGBA) {
                return Reject(TVPLayerGPURejectReason::SourceFormat);
            }
            int sw=src.get_width(),sh=src.get_height(),dw=dst.get_width(),dh=dst.get_height();
            if(sw==0 || sh==0 || dw<=0 || dh<=0) return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            if(op.kind==TVPLayerOperationKind::BoxBlur &&
                (sw!=dw || sh!=dh || src.left<0 || src.top<0 || src.right>int(source->GetWidth()) ||
                 src.bottom>int(source->GetHeight()) || dst.left<0 || dst.top<0 ||
                 dst.right>int(t->GetWidth()) || dst.bottom>int(t->GetHeight())))
                return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            if(sw>0 && sh>0 && (sw!=dw || sh!=dh)) {
                // Match the software manager's integer source adjustments before resize.
                if(dst.left<0) { src.left+=float(sw)/dw*-dst.left; dst.left=0; }
                if(dst.right>int(t->GetWidth())) { src.right-=float(src.get_width())/dst.get_width()*(dst.right-t->GetWidth()); dst.right=t->GetWidth(); }
                if(dst.top<0) { src.top+=float(sh)/dh*-dst.top; dst.top=0; }
                if(dst.bottom>int(t->GetHeight())) { src.bottom-=float(src.get_height())/dst.get_height()*(dst.bottom-t->GetHeight()); dst.bottom=t->GetHeight(); }
                if(src.get_width()==0 || src.get_height()==0 || dst.get_width()<=0 || dst.get_height()<=0) return true;
            } else if((sw<0 || sh<0) && (std::abs(sw)!=dw || std::abs(sh)!=dh)) {
                return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            }
        } else if(!(traits->logicalInputCountMask & 1)) {
            return Reject(TVPLayerGPURejectReason::UnsupportedKind);
        }
        if(!session->tablesReady) {
            if(!session->backend->SetLayerAlphaTables(TVPOpacityOnOpacityTable,TVPNegativeMulTable))
                return Reject(TVPLayerGPURejectReason::AlphaTables);
            session->tablesReady=true;
        }
        void* sh=source?source->GetTextureHandle():nullptr;
        if((traits->parameterResources&TVP_LAYER_RESOURCE_PS_TABLES) && !session->psTablesReady) {
            if(!session->backend->SetLayerPsTables(TVPGetPsBlendTable(0),TVPGetPsBlendTable(1),TVPGetPsBlendTable(2)))
                return Reject(TVPLayerGPURejectReason::PsTables);
            session->psTablesReady=true;
        }
        if(!session->backend->OperateLayerRect(op,t->GetTextureHandle(),Rect(dst),sh,Rect(src),stretch==0?0:1))
            return Reject(TVPLayerGPURejectReason::BackendFailure);
        // Only plain HDA blending preserves destination alpha. The _d/_a
        // formulas can change it even if HOLD_ALPHA is also set.
        const bool preservesAlpha=TVPLayerOperationPreservesAlpha(op,source==t);
        t->InvalidateCPUCacheRegion(dst,preservesAlpha); ++session->stats.gpuOperations; return true;
    }
    void OperateRect(iTVPRenderMethod* method,iTVPTexture2D* target,iTVPTexture2D* reference,const tTVPRect& dst,const tRenderTexRectArray& inputs) override {
        // Software's generic resize primitive assumes RGBA byte addressing.
        // R8 mask scaling/mirroring has no safe software reference path.
        TVPLayerOperation requested;
        if(method && method->DescribeGpuOperation(requested) &&
           (TVPLayerOperationRequiresForwardSource(requested.kind) || requested.kind==TVPLayerOperationKind::AdjustGamma ||
            (requested.kind==TVPLayerOperationKind::ConstAlphaSD && (requested.flags&TVP_LAYER_DEST_PREMULTIPLIED)))) {
            // These new software references write uint32 pixels. Invalid R8/RGB
            // targets or sources must not reach a 32-bit software fallback.
            if(!target || target->GetFormat()!=TVPTextureFormat::RGBA) {
                Reject(TVPLayerGPURejectReason::SourceFormat);
                TVPThrowExceptionMessage(TJS_N("This Layer method requires an RGBA target."));
            }
            if(requested.kind==TVPLayerOperationKind::RemoveOpacity) {
                for(size_t i=0;i<inputs.size();++i) if(!inputs[i].first ||
                   (inputs[i].first->GetFormat()!=TVPTextureFormat::Gray && inputs[i].first->GetFormat()!=TVPTextureFormat::RGBA)) {
                    Reject(TVPLayerGPURejectReason::SourceFormat);
                    TVPThrowExceptionMessage(TJS_N("RemoveOpacity requires an R8 mask or a software-compatible RGBA input."));
                }
            } else {
                for(size_t i=0;i<inputs.size();++i) if(!inputs[i].first || inputs[i].first->GetFormat()!=TVPTextureFormat::RGBA) {
                    Reject(TVPLayerGPURejectReason::SourceFormat);
                    TVPThrowExceptionMessage(TJS_N("This Layer method requires RGBA inputs."));
                }
                if(requested.kind==TVPLayerOperationKind::AdditiveAlphaToAlpha && inputs.size()==0 &&
                   (!reference || reference->GetFormat()!=TVPTextureFormat::RGBA)) {
                    Reject(TVPLayerGPURejectReason::SourceFormat);
                    TVPThrowExceptionMessage(TJS_N("Alpha conversion requires an RGBA reference."));
                }
            }
        }
        if(method && method->DescribeGpuOperation(requested) &&
           TVPLayerOperationRequiresForwardSource(requested.kind) && inputs.size()==1 &&
           (inputs[0].second.get_width()<=0 || inputs[0].second.get_height()<=0)) {
            Reject(TVPLayerGPURejectReason::InvalidGeometry);
            TVPThrowExceptionMessage(TJS_N("This Layer method requires a forward source rectangle."));
        }
        if(method && method->DescribeGpuOperation(requested) && requested.kind==TVPLayerOperationKind::RemoveOpacity &&
           inputs.size()==1 && inputs[0].first && inputs[0].first->GetFormat()==TVPTextureFormat::Gray) {
            const auto& source=inputs[0].second;
            if(source.get_width()!=dst.get_width() || source.get_height()!=dst.get_height() ||
               source.get_width()<=0 || source.get_height()<=0 || source.left<0 || source.top<0 ||
               source.right>int(inputs[0].first->GetWidth()) || source.bottom>int(inputs[0].first->GetHeight()) ||
               !target || dst.left<0 || dst.top<0 || dst.right>int(target->GetWidth()) || dst.bottom>int(target->GetHeight())) {
                Reject(TVPLayerGPURejectReason::InvalidGeometry);
                TVPThrowExceptionMessage(TJS_N("RemoveOpacity requires equal-size, in-bounds R8 mask rectangles without mirroring."));
            }
        }
        if(GPU(method,target,reference,dst,inputs)) {
            krkrsdl3::layer_work::RecordTransitionResult(true,"render.gpu",
                dst.get_width()>0 && dst.get_height()>0 ? uint64_t(dst.get_width())*dst.get_height() : 0);
            return;
        }
        tTVPRect fallbackDst=dst;
        TVPLayerOperation op;
        const bool univTrans=target && inputs.size()==3 && method->DescribeGpuOperation(op) &&
                             op.kind==TVPLayerOperationKind::UnivTrans;
        if(univTrans) {
            // The software three-source primitive expects pre-clipped rectangles.
            // Preserve the GPU path's source/rule offsets when a CPU target or
            // an unavailable pipeline forces fallback. Empty clips need no views
            // (and therefore no GPU readback or dirty CPU cache).
            fallbackDst=tTVPRect(std::max(0,dst.left),std::max(0,dst.top),
                                std::min(int(target->GetWidth()),dst.right),
                                std::min(int(target->GetHeight()),dst.bottom));
            if(fallbackDst.get_width()<=0 || fallbackDst.get_height()<=0) return;
        }
        CPUViews views; std::vector<std::pair<iTVPTexture2D*,tTVPRect>> textures;
        krkrsdl3::layer_work::SourceScope source(method ? method->GetName().c_str() : "fallback.unnamed");
        krkrsdl3::layer_work::StageScope software(krkrsdl3::layer_work::Stage::Software);
        auto* targetView=views.Get(target,TVPLayerFallbackReadbackRole::Target);
        auto* referenceView=views.Get(reference,TVPLayerFallbackReadbackRole::Reference);
        for(size_t i=0;i<inputs.size();++i) {
            auto src=inputs[i].second;
            if(univTrans) {
                src.left+=fallbackDst.left-dst.left; src.top+=fallbackDst.top-dst.top;
                src.right-=dst.right-fallbackDst.right; src.bottom-=dst.bottom-fallbackDst.bottom;
            }
            textures.emplace_back(views.Get(inputs[i].first,TVPLayerFallbackReadbackRole::Source),src);
        }
        Software()->OperateRect(method,targetView,referenceView,fallbackDst,tRenderTexRectArray(textures.data(),textures.size()));
        if(target) target->MarkCPUModified(fallbackDst);
        if(session) ++session->stats.cpuFallbacks;
    }
    bool GPUAffine(iTVPRenderMethod* method,int count,iTVPTexture2D* target,
                       const tTVPRect& clip,const tTVPPointD* points,const tRenderTexQuadArray& inputs) {
        auto* t=dynamic_cast<LayerTexture*>(target);
        if(!session || !t || !t->Belongs(session)) return Reject(TVPLayerGPURejectReason::TargetUnavailable);
        if(t->IsCPUResident()) return Reject(TVPLayerGPURejectReason::TargetCPUResident);
        if(count!=2) return Reject(TVPLayerGPURejectReason::InvalidGeometry);
        if(inputs.size()!=1) return RejectMethod(TVPLayerGPURejectReason::MultipleInputs,method,inputs.size());
        if(stretch<0 || stretch>2) return Reject(TVPLayerGPURejectReason::UnsupportedStretch);
        auto* s=dynamic_cast<LayerTexture*>(inputs[0].first);
        TVPLayerOperation op;
        if(!method || !method->DescribeGpuOperation(op)) return RejectMethod(TVPLayerGPURejectReason::UnsupportedMethod,method);
        const auto* traits=TVPGetLayerOperationTraits(op.kind);
        if(!traits) return Reject(TVPLayerGPURejectReason::UnsupportedKind);
        if(op.kind!=TVPLayerOperationKind::Copy && (op.opacity<0 || op.opacity>255))
            return Reject(TVPLayerGPURejectReason::InvalidOpacity);
        if(!TVPLayerOperationSupportsAffine(op)) return Reject(TVPLayerGPURejectReason::UnsupportedKind);
        if(!s || !s->Belongs(session)) return RejectSource(method,inputs[0].first);
        if(t->GetFormat()!=TVPTextureFormat::RGBA || s->GetFormat()!=TVPTextureFormat::RGBA)
            return Reject(TVPLayerGPURejectReason::SourceFormat);
        TVPLayerAffineCopy affine;
        // The software warp uses clip-relative coordinates. Require a clip
        // already inside the target; LayerBitmap clamps it before dispatch.
        if(clip.left<0 || clip.top<0 || clip.right>int(t->GetWidth()) || clip.bottom>int(t->GetHeight()))
            return Reject(TVPLayerGPURejectReason::InvalidGeometry);
        if(!layer_affine::Prepare(points,inputs[0].second,s->GetWidth(),s->GetHeight(),Rect(clip),affine))
            return Reject(TVPLayerGPURejectReason::InvalidGeometry);
        const auto* srcpt=inputs[0].second;
        const auto close=[](double a,double b) { return std::abs(a-b)<0.001; };
        const bool rectangle=close(points[0].y,points[1].y) && close(points[1].x,points[5].x) &&
                             close(points[0].x,points[2].x) && close(points[2].y,points[5].y);
        // WarpAffine builds its complete temporary before DoRender reads target.
        // It ignores reference, including COW callers with a distinct old image.
        // The rectangular shortcut instead retains scanline overlap semantics.
        const bool copy=op.kind==TVPLayerOperationKind::Copy;
        auto resources=[&]() {
            if(copy) return true;
            if(!session->tablesReady) {
                if(!session->backend->SetLayerAlphaTables(TVPOpacityOnOpacityTable,TVPNegativeMulTable))
                    return Reject(TVPLayerGPURejectReason::AlphaTables);
                session->tablesReady=true;
            }
            if((traits->parameterResources&TVP_LAYER_RESOURCE_PS_TABLES) && !session->psTablesReady) {
                if(!session->backend->SetLayerPsTables(TVPGetPsBlendTable(0),TVPGetPsBlendTable(1),TVPGetPsBlendTable(2)))
                    return Reject(TVPLayerGPURejectReason::PsTables);
                session->psTablesReady=true;
            }
            return true;
        };
        tTVPRect affected=clip;
        if(rectangle) {
            tTVPRect dst(points[0].x,points[0].y,points[5].x,points[5].y);
            tTVPRect src(srcpt[0].x,srcpt[0].y,srcpt[5].x,srcpt[5].y);
            // Mirrored axis-aligned quads use the old fixed-point scanline
            // rasterizer. Keep that distinct behavior in software for now.
            if(dst.get_width()<=0 || dst.get_height()<=0) return Reject(TVPLayerGPURejectReason::InvalidGeometry);
            if(!TVPIntersectRect(&affected,clip,dst)) affected=tTVPRect(0,0,0,0);
            if(affected.get_width()>0 && affected.get_height()>0) {
                const int dw=dst.get_width(),dh=dst.get_height(),sw=src.get_width(),sh=src.get_height();
                if(affected.left!=dst.left) src.left+=(float)sw/dw*(affected.left-dst.left);
                if(affected.top!=dst.top) src.top+=(float)sh/dh*(affected.top-dst.top);
                if(affected.right!=dst.right) src.right-=(float)sw/dw*(dst.right-affected.right);
                if(affected.bottom!=dst.bottom) src.bottom-=(float)sh/dh*(dst.bottom-affected.bottom);
                if(src.get_width()<=0 || src.get_height()<=0) return Reject(TVPLayerGPURejectReason::InvalidGeometry);
                if(!copy && s==t && (src.left!=affected.left || src.top!=affected.top ||
                   src.right!=affected.right || src.bottom!=affected.bottom))
                    return Reject(TVPLayerGPURejectReason::AffineAlias);
                if(!resources()) return false;
                if(!session->backend->OperateLayerRect(op,t->GetTextureHandle(),Rect(affected),s->GetTextureHandle(),Rect(src),stretch==0?0:1))
                    return Reject(TVPLayerGPURejectReason::BackendFailure);
            }
        } else if(clip.get_width()>0 && clip.get_height()>0) {
            if(!resources()) return false;
            if(!session->backend->OperateLayerAffine(op,t->GetTextureHandle(),affine,s->GetTextureHandle(),stretch==0?0:1))
                return Reject(TVPLayerGPURejectReason::BackendFailure);
        } else affected=tTVPRect(0,0,0,0);
        t->InvalidateCPUCacheRegion(affected,TVPLayerOperationPreservesAlpha(op,s==t));
        ++session->stats.gpuOperations;
        if(triangle_trace::Enabled()) {
            ++session->triangles.stats.gpuCalls;
            session->triangles.stats.gpuPixels+=uint64_t(affected.get_width())*affected.get_height();
        }
        return true;
    }
    void OperateTriangles(iTVPRenderMethod* method,int count,iTVPTexture2D* target,iTVPTexture2D* reference,const tTVPRect& clip,const tTVPPointD* points,const tRenderTexQuadArray& inputs) override {
        if(GPUAffine(method,count,target,clip,points,inputs)) return;
        Reject(TVPLayerGPURejectReason::Triangles);
        krkrsdl3::layer_work::SourceScope source(method ? method->GetName().c_str() : "fallback.triangles");
        krkrsdl3::layer_work::StageScope work(krkrsdl3::layer_work::Stage::Software);
        const bool profile=session && triangle_trace::Enabled();
        const auto start=profile ? TriangleClock::now() : TriangleClock::time_point{};
        uint64_t beforeBytes[static_cast<int>(TVPLayerFallbackReadbackRole::Count)] = {};
        if(profile) std::copy(std::begin(session->stats.fallbackReadbackBytesByRole),
                              std::end(session->stats.fallbackReadbackBytesByRole),beforeBytes);
        uint64_t softwareNS=0;
        {
            CPUViews views; std::vector<std::pair<iTVPTexture2D*,const tTVPPointD*>> textures;
            auto* targetView=views.Get(target,TVPLayerFallbackReadbackRole::Target);
            auto* referenceView=views.Get(reference,TVPLayerFallbackReadbackRole::Reference);
            for(size_t i=0;i<inputs.size();++i)
                textures.emplace_back(views.Get(inputs[i].first,TVPLayerFallbackReadbackRole::Source),inputs[i].second);
            const auto softwareStart=profile ? TriangleClock::now() : TriangleClock::time_point{};
            Software()->OperateTriangles(method,count,targetView,referenceView,clip,points,tRenderTexQuadArray(textures.data(),textures.size()));
            if(profile) softwareNS=ElapsedNS(softwareStart,TriangleClock::now());
            if(target) target->MarkCPUModified(); if(session) ++session->stats.cpuFallbacks;
        }
        if(profile) {
            auto& interval=session->triangles;
            auto& stats=interval.stats;
            const uint64_t cpuNS=ElapsedNS(start,TriangleClock::now());
            ++stats.calls;
            stats.triangleCount+=std::max(0,count);
            stats.cpuTimeNS+=cpuNS; stats.maxCpuTimeNS=std::max(stats.maxCpuTimeNS,cpuNS);
            stats.softwareTimeNS+=softwareNS; stats.maxSoftwareTimeNS=std::max(stats.maxSoftwareTimeNS,softwareNS);
            auto bytes=[&](TVPLayerFallbackReadbackRole role) {
                const int index=static_cast<int>(role);
                return session->stats.fallbackReadbackBytesByRole[index]-beforeBytes[index];
            };
            stats.targetReadbackBytes+=bytes(TVPLayerFallbackReadbackRole::Target);
            stats.sourceReadbackBytes+=bytes(TVPLayerFallbackReadbackRole::Source);
            stats.referenceReadbackBytes+=bytes(TVPLayerFallbackReadbackRole::Reference);
            stats.count2Calls+=count==2;
            stats.singleInputCalls+=inputs.size()==1;
            stats.referenceCalls+=reference!=nullptr;
            bool alias=false;
            for(size_t i=0;i<inputs.size();++i) alias|=target && inputs[i].first==target;
            stats.sourceTargetAliasCalls+=alias;
            ++interval.sources[static_cast<int>(triangle_trace::source)];
            if(target) {
                const int64_t w=target->GetWidth(),h=target->GetHeight();
                const uint64_t area=uint64_t(w)*h;
                const uint64_t visible=uint64_t(std::max<int64_t>(0,std::min<int64_t>(w,clip.right)-std::max<int64_t>(0,clip.left)))*
                                       std::max<int64_t>(0,std::min<int64_t>(h,clip.bottom)-std::max<int64_t>(0,clip.top));
                stats.clipPixels+=visible; stats.maxClipPixels=std::max(stats.maxClipPixels,visible);
                stats.maxTargetPixels=std::max(stats.maxTargetPixels,area);
                stats.fullSurfaceCalls+=area && visible==area;
                stats.target1920x1080Calls+=w==1920 && h==1080;
            }
            try {
                interval.methods.Add(method && !method->GetName().empty() ? method->GetName() : "<unnamed>");
                interval.targetSizes.Add(target ? std::to_string(target->GetWidth())+"x"+std::to_string(target->GetHeight()) : "none");
                interval.stretchModes.Add(std::to_string(stretch));
            } catch(...) {
                // Diagnostic allocation must not change rendering behavior.
            }
        }
    }
    bool GPUPerspective(iTVPRenderMethod* method,iTVPTexture2D* target,iTVPTexture2D* source,
                        const std::vector<TVPLayerPerspectiveQuad>& quads,const tTVPRect& affected) {
        auto* t=dynamic_cast<LayerTexture*>(target);
        if(!session || !t || !t->Belongs(session)) return Reject(TVPLayerGPURejectReason::TargetUnavailable);
        if(t->IsCPUResident()) return Reject(TVPLayerGPURejectReason::TargetCPUResident);
        if(stretch<0 || stretch>2) return Reject(TVPLayerGPURejectReason::UnsupportedStretch);
        if(quads.size()>TVP_LAYER_PERSPECTIVE_MAX_QUADS) return Reject(TVPLayerGPURejectReason::InvalidGeometry);
        TVPLayerOperation op;
        if(!method || !method->DescribeGpuOperation(op)) return RejectMethod(TVPLayerGPURejectReason::UnsupportedMethod,method);
        if(op.kind!=TVPLayerOperationKind::Copy && (op.opacity<0 || op.opacity>255))
            return Reject(TVPLayerGPURejectReason::InvalidOpacity);
        const auto* traits=TVPGetLayerOperationTraits(op.kind);
        if(!traits || !TVPLayerOperationSupportsPerspective(op)) return Reject(TVPLayerGPURejectReason::UnsupportedKind);
        auto* s=dynamic_cast<LayerTexture*>(source);
        if(!s || !s->Belongs(session)) return RejectSource(method,source);
        if(s==t) for(const auto& q:quads) if(q.rectangle && q.clip.Width()>0 && q.clip.Height()>0 &&
           (q.source.left!=q.destination.left || q.source.top!=q.destination.top ||
            q.source.right!=q.destination.right || q.source.bottom!=q.destination.bottom))
            return Reject(TVPLayerGPURejectReason::PerspectiveAlias);
        if(op.kind!=TVPLayerOperationKind::Copy && !session->tablesReady) {
            if(!session->backend->SetLayerAlphaTables(TVPOpacityOnOpacityTable,TVPNegativeMulTable))
                return Reject(TVPLayerGPURejectReason::AlphaTables);
            session->tablesReady=true;
        }
        if((traits->parameterResources&TVP_LAYER_RESOURCE_PS_TABLES) && !session->psTablesReady) {
            if(!session->backend->SetLayerPsTables(TVPGetPsBlendTable(0),TVPGetPsBlendTable(1),TVPGetPsBlendTable(2)))
                return Reject(TVPLayerGPURejectReason::PsTables);
            session->psTablesReady=true;
        }
        if(!session->backend->OperateLayerPerspective(op,t->GetTextureHandle(),quads.data(),quads.size(),
                                                      s->GetTextureHandle(),stretch==0?0:1))
            return Reject(TVPLayerGPURejectReason::BackendFailure);
        t->InvalidateCPUCacheRegion(affected,TVPLayerOperationPreservesAlpha(op,s==t));
        ++session->stats.gpuOperations;
        return true;
    }
    void OperatePerspective(iTVPRenderMethod* method,int count,iTVPTexture2D* target,iTVPTexture2D* reference,const tTVPRect& clip,const tTVPPointD* points,const tRenderTexQuadArray& inputs) override {
        // Validate the entire call before acquiring pixels or encoding its first
        // quad. A late malformed quad must not replay an already written prefix.
        if(count<0 || !method || !target || inputs.size()!=1 ||
           !inputs[0].first || (count>0 && (!points || !inputs[0].second))) {
            Reject(inputs.size()!=1?TVPLayerGPURejectReason::MultipleInputs:TVPLayerGPURejectReason::InvalidGeometry);
            TVPThrowExceptionMessage(TJS_N("Perspective requires a method, target and one complete quad input."));
        }
        if(target->GetFormat()!=TVPTextureFormat::RGBA || inputs[0].first->GetFormat()!=TVPTextureFormat::RGBA) {
            Reject(TVPLayerGPURejectReason::SourceFormat);
            TVPThrowExceptionMessage(TJS_N("Perspective requires RGBA source and target textures."));
        }
        if(stretch<0) {
            Reject(TVPLayerGPURejectReason::UnsupportedStretch);
            TVPThrowExceptionMessage(TJS_N("Perspective requires a nonnegative StretchType."));
        }
        if(!count) return;
        const tTVPRect bounded(std::max(0,clip.left),std::max(0,clip.top),
            std::min(int(target->GetWidth()),clip.right),std::min(int(target->GetHeight()),clip.bottom));
        if(bounded.get_width()<=0 || bounded.get_height()<=0) return;
        std::vector<TVPLayerPerspectiveQuad> quads(static_cast<std::size_t>(count));
        tTVPRect affected; bool written=false;
        for(int i=0;i<count;++i) {
            if(!layer_perspective::PrepareQuad(points+std::size_t(i)*4,inputs[0].second+std::size_t(i)*4,
                    inputs[0].first->GetWidth(),inputs[0].first->GetHeight(),Rect(bounded),quads[i])) {
                Reject(TVPLayerGPURejectReason::InvalidGeometry);
                TVPThrowExceptionMessage(TJS_N("Perspective coordinates must be finite and define a nonsingular mapping."));
            }
            const auto& r=quads[i].clip;
            if(r.Width()<=0 || r.Height()<=0) continue;
            if(!written) {affected=tTVPRect(r.left,r.top,r.right,r.bottom);written=true;}
            else affected=tTVPRect(std::min(affected.left,r.left),std::min(affected.top,r.top),
                                  std::max(affected.right,r.right),std::max(affected.bottom,r.bottom));
        }
        if(!written) return;
        if(GPUPerspective(method,target,inputs[0].first,quads,affected)) return;
        Reject(TVPLayerGPURejectReason::Perspective);
        krkrsdl3::layer_work::SourceScope source(method ? method->GetName().c_str() : "fallback.perspective");
        krkrsdl3::layer_work::StageScope work(krkrsdl3::layer_work::Stage::Software);
        CPUViews views; std::vector<std::pair<iTVPTexture2D*,const tTVPPointD*>> textures;
        auto* targetView=views.Get(target,TVPLayerFallbackReadbackRole::Target);
        auto* referenceView=views.Get(reference,TVPLayerFallbackReadbackRole::Reference);
        for(size_t i=0;i<inputs.size();++i)
            textures.emplace_back(views.Get(inputs[i].first,TVPLayerFallbackReadbackRole::Source),inputs[i].second);
        Software()->OperatePerspective(method,count,targetView,referenceView,bounded,points,tRenderTexQuadArray(textures.data(),textures.size()));
        target->MarkCPUModified(affected); if(session) ++session->stats.cpuFallbacks;
    }
};
LayerManager& Manager() { static auto* manager=new LayerManager; return *manager; }
}
bool TVPBindMetalLayerRenderManager(krkrsdl3::iTVPRenderBackend* backend) {
    TVPUnbindMetalLayerRenderManager();
    fallbackReason.clear();
    if(krkrsdl3::layer_work::enabled.load(std::memory_order_relaxed)) krkrsdl3::layer_work::SetEnabled(true);
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
bool TVPHasMetalLayerTransitionSupport() {
    const auto& session=Manager().session;
    return session && session->backend && session->backend->SupportsLayerTransitions();
}
bool TVPHasMetalLayerShrinkSupport() {
    const auto& s=Manager().session;
    return s && s->backend && s->backend->SupportsLayerShrinks();
}
TVPLayerShrinkResult TVPTryMetalLayerShrink(const TVPLayerShrinkOperation& operation,
        iTVPTexture2D* target,iTVPTexture2D* retainedSource) {
    using Result=TVPLayerShrinkResult;
    const auto& rect=operation.destination;
    const int64_t w=int64_t(rect.right)-rect.left,h=int64_t(rect.bottom)-rect.top;
    const uint64_t pixels=w>0 && h>0 ? uint64_t(w)*uint64_t(h) : 0;
    auto failure=[&](Result result,const char* reason) {
        krkrsdl3::layer_work::RecordShrinkResult(false,reason,pixels,
            result==Result::AliasDependency ? "selfDependent" : nullptr); return result;
    };
    if(operation.kind!=TVPLayerShrinkKind::Area && operation.kind!=TVPLayerShrinkKind::Fast)
        return failure(Result::Unsupported,"unsupportedKind");
    auto& session=Manager().session;
    if(!session || !session->backend || !session->backend->SupportsLayerShrinks()) return failure(Result::BackendFailure,"pipelineUnavailable");
    auto* t=dynamic_cast<LayerTexture*>(target);auto* s=dynamic_cast<LayerTexture*>(retainedSource);
    if(!t || !s || !t->Belongs(session) || !s->Belongs(session)) return failure(Result::Resource,"sessionOrResource");
    if(t->GetFormat()!=TVPTextureFormat::RGBA || s->GetFormat()!=TVPTextureFormat::RGBA) return failure(Result::Resource,"format");
    if(t->IsCPUResident() || s->IsCPUResident() || t->HasCPUAccess() || s->HasCPUAccess()) return failure(Result::CPUAccess,"cpuLease");
    auto checked=TVPLayerShrinkGeometry::Validate(operation,t->GetWidth(),t->GetHeight(),s->GetWidth(),s->GetHeight(),t==s);
    if(checked!=Result::Applied) return failure(checked,checked==Result::AliasDependency ? "aliasDependency" :
        checked==Result::Arithmetic ? "arithmetic" : checked==Result::ParameterBudget ? "parameterBudget" : "geometry");
    if(!pixels) {krkrsdl3::layer_work::RecordShrinkResult(false,"noop",0);return Result::Applied;}
    if(operation.avgBits==64 && !TVPLayerShrinkGeometry::CanUse32(operation) && !session->backend->SupportsLayerShrink64())
        return failure(Result::Arithmetic,"integerWidth");
    const bool full=rect.left==0 && rect.top==0 && rect.right==int(t->GetWidth()) && rect.bottom==int(t->GetHeight());
    // Upload necessary source CPU damage first; on real alias that is also the
    // target's pre-operation input, which a full overwrite must not discard.
    void* sh=s->GetTextureHandle();
    void* th=full ? t->GetTextureHandleForOverwrite() : t->GetTextureHandleForRegionWrite();
    if(!sh || !th) return failure(Result::Resource,"textureHandle");
    bool applied=false;
    try {applied=session->backend->OperateLayerShrink(operation,th,sh);}
    catch(...) {
        if(session->backend->LastLayerShrinkResult()==Result::Applied) {
            t->CommitGPURegionWrite(tTVPRect(rect.left,rect.top,rect.right,rect.bottom));
            krkrsdl3::layer_work::RecordShrinkResult(true,"postCommitFailure",pixels,t==s ? "selfSafe" : nullptr);
        }
        throw;
    }
    if(!applied) {
        auto result=session->backend->LastLayerShrinkResult();
        return failure(result,result==Result::AliasDependency ? "aliasDependency" : result==Result::Arithmetic ? "arithmetic" :
            result==Result::ParameterBudget ? "parameterBudget" : result==Result::Geometry ? "geometry" : "backendFailure");
    }
    t->CommitGPURegionWrite(tTVPRect(rect.left,rect.top,rect.right,rect.bottom));
    krkrsdl3::layer_work::RecordShrinkResult(true,"gpu",pixels,t==s ? "selfSafe" : nullptr);
    krkrsdl3::cpu_consumer_trace::ReportShrinkOutput(t,rect.left,rect.top,rect.right,rect.bottom);
    return Result::Applied;
}
TVPLayerTransitionResult TVPTryMetalLayerTransition(const TVPLayerTransitionOperation& operation,
        iTVPTexture2D* target,iTVPTexture2D* source1,iTVPTexture2D* source2) {
    using Result=TVPLayerTransitionResult;
    const auto& p=operation.params;
    const uint64_t pixels=p.width>0 && p.height>0 ? uint64_t(p.width)*p.height : 0;
    auto fail=[&](Result r,const char* reason) {
        krkrsdl3::layer_work::RecordTransitionResult(false,reason,pixels); return r;
    };
    if(p.kind<=0 || p.kind>=int(TVPLayerTransitionKind::Count)) return fail(Result::Unsupported,"unsupportedKind");
    auto& session=Manager().session;
    if(!session || !session->backend || !session->backend->SupportsLayerTransitions())
        return fail(Result::PipelineUnavailable,"pipelineUnavailable");
    auto* t=dynamic_cast<LayerTexture*>(target);
    auto* s1=dynamic_cast<LayerTexture*>(source1);
    auto* s2=dynamic_cast<LayerTexture*>(source2);
    if(!t || !s1 || !s2 || !t->Belongs(session) || !s1->Belongs(session) || !s2->Belongs(session))
        return fail(Result::InvalidResource,"sessionOrResource");
    if(t->GetFormat()!=TVPTextureFormat::RGBA || s1->GetFormat()!=TVPTextureFormat::RGBA ||
       s2->GetFormat()!=TVPTextureFormat::RGBA) return fail(Result::InvalidResource,"format");
    if(t==s1 || t==s2) return fail(Result::Alias,"targetAlias");
    if(t->IsCPUResident() || s1->IsCPUResident() || s2->IsCPUResident() ||
       t->HasCPUAccess() || s1->HasCPUAccess() || s2->HasCPUAccess()) return fail(Result::CPUAccess,"cpuLease");
    const auto checked=layer_transition::Validate(operation,t->GetWidth(),t->GetHeight(),
        s1->GetWidth(),s1->GetHeight(),s2->GetWidth(),s2->GetHeight(),false);
    if(checked!=Result::Applied) return fail(checked,checked==Result::InvalidGeometry ? "geometry" :
        checked==Result::AllocationFailed ? "parameterBudget" : "parameters");
    if(!pixels) { krkrsdl3::layer_work::RecordTransitionResult(false,"passthrough",0); return Result::Applied; }
    if(p.kind==int(TVPLayerTransitionKind::Wave) && p.flags==1 && !session->tablesReady) {
        if(!session->backend->SetLayerAlphaTables(TVPOpacityOnOpacityTable,TVPNegativeMulTable))
            return fail(Result::AllocationFailed,"alphaTables");
        session->tablesReady=true;
    }
    // Dirty CPU damage is uploaded by the existing texture transaction. A full
    // overwrite skips fetching/uploading the destination's discarded old pixels.
    const bool full=p.destLeft==0 && p.destTop==0 && p.width==int(t->GetWidth()) && p.height==int(t->GetHeight());
    void* th=full ? t->GetTextureHandleForOverwrite() : t->GetTextureHandleForRegionWrite();
    void* h1=s1->GetTextureHandle(); void* h2=s2->GetTextureHandle();
    if(!th || !h1 || !h2) return fail(Result::InvalidResource,"textureHandle");
    if(!session->backend->OperateLayerTransition(operation,th,h1,h2)) {
        const auto result=session->backend->LastLayerTransitionResult();
        return fail(result,result==Result::AllocationFailed ? "allocationFailed" :
            result==Result::InvalidGeometry ? "geometry" : result==Result::InvalidParameters ? "parameters" :
            result==Result::Unsupported ? "unsupportedKind" : result==Result::Alias ? "targetAlias" : "backendRejected");
    }
    t->CommitGPURegionWrite(tTVPRect(p.destLeft,p.destTop,p.destLeft+p.width,p.destTop+p.height));
    krkrsdl3::layer_work::RecordTransitionResult(true,"gpu",pixels);
    return Result::Applied;
}
TVPLayerRenderStats TVPGetMetalLayerRenderStats() {
    const auto& session=Manager().session;
    if(!session) return {};
    auto stats=session->stats;
    const auto parameters=session->backend->GetLayerParameterUploadStats();
    stats.gammaLUTUploads=parameters.gammaLUTUploads-session->parameterBaseline.gammaLUTUploads;
    stats.gammaLUTUploadedBytes=parameters.gammaLUTUploadedBytes-session->parameterBaseline.gammaLUTUploadedBytes;
    stats.psTableUploads=parameters.psTableUploads-session->parameterBaseline.psTableUploads;
    stats.psTableUploadedBytes=parameters.psTableUploadedBytes-session->parameterBaseline.psTableUploadedBytes;
    return stats;
}

void TVPSetMetalLayerTriangleDiagnostics(bool enabled) {
    if(triangle_trace::Enabled()==enabled) return;
    triangle_trace::enabled.store(enabled,std::memory_order_relaxed);
    if(Manager().session) Manager().session->triangles=TriangleInterval{};
}
TVPLayerTriangleProfile TVPTakeMetalLayerTriangleProfile() {
    auto& manager=Manager();
    if(!manager.session || !triangle_trace::Enabled()) return {};
    auto& interval=manager.session->triangles;
    const auto sampled=TriangleClock::now();
    TVPLayerTriangleProfile profile;
    profile.stats=interval.stats;
    profile.stats.intervalNS=ElapsedNS(interval.started,sampled);
    profile.methods=interval.methods.Summary();
    profile.targetSizes=interval.targetSizes.Summary();
    profile.stretchModes=interval.stretchModes.Summary();
    for(int i=0;i<static_cast<int>(triangle_trace::Source::Count);++i) {
        if(!interval.sources[i]) continue;
        if(!profile.sources.empty()) profile.sources+=',';
        profile.sources+=std::string(triangle_trace::Name(static_cast<triangle_trace::Source>(i)))+":"+std::to_string(interval.sources[i]);
    }
    interval=TriangleInterval{};
    interval.started=sampled;
    return profile;
}

static std::string FormatMethodSummary(const std::unordered_map<std::string,uint64_t>& methods) {
    std::vector<std::pair<std::string,uint64_t>> ordered(methods.begin(),methods.end());
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) {
        return a.second!=b.second ? a.second>b.second : a.first<b.first;
    });
    constexpr size_t limit=8;
    std::string out;
    for(size_t i=0;i<ordered.size() && i<limit;++i) {
        if(!out.empty()) out.push_back(',');
        out+=ordered[i].first+":"+std::to_string(ordered[i].second);
    }
    if(ordered.size()>limit) out+=",otherMethods:"+std::to_string(ordered.size()-limit);
    return out;
}
std::string TVPGetMetalLayerMultipleInputMethodSummary() {
    auto& manager=Manager();
    return manager.session?FormatMethodSummary(manager.session->multipleInputMethods):std::string();
}
std::string TVPGetMetalLayerUnsupportedMethodSummary() {
    auto& manager=Manager();
    return manager.session?FormatMethodSummary(manager.session->unsupportedMethods):std::string();
}

const char* TVPMetalLayerFallbackReason() { return fallbackReason.c_str(); }

void TVPMarkEmoteAlphaTexture(iTVPTexture2D* texture) {
    if(auto* metal=dynamic_cast<LayerTexture*>(texture)) metal->MarkEmoteAlpha();
}
bool TVPIsEmoteAsyncAlphaTexture(iTVPTexture2D* texture) {
    auto* metal=dynamic_cast<LayerTexture*>(texture);
    return metal && metal->HasEmoteAlpha() &&
        emoteplayer::performanceEnabled("MIKAGE_EMOTE_ASYNC_ALPHA");
}
bool TVPRequestEmoteAsyncAlpha(iTVPTexture2D* texture,int x,int y,
                             std::shared_ptr<krkrsdl3::AsyncAlphaTile>& tile) {
    if(!TVPIsEmoteAsyncAlphaTexture(texture)) return false;
    tile=static_cast<LayerTexture*>(texture)->DemandAlpha(x,y);
    return true;
}
namespace { unsigned alphaPresentationDepth=0; }
void TVPBeginEmoteAlphaPresentation() { ++alphaPresentationDepth; }
void TVPEndEmoteAlphaPresentation() { if(alphaPresentationDepth) --alphaPresentationDepth; }
void TVPEncodeEmoteAsyncAlphaForPresentation(iTVPTexture2D* texture) {
    if(!alphaPresentationDepth) return;
    if(auto* metal=dynamic_cast<LayerTexture*>(texture)) metal->EncodeAlphaForPresentation();
}
void TVPEncodeFrozenEmoteAsyncAlpha(iTVPTexture2D* texture,
    const std::shared_ptr<krkrsdl3::AsyncLayerPresentation>& presentation) {
    if(!presentation || presentation->failed.load(std::memory_order_acquire)) return;
    if(auto* metal=dynamic_cast<LayerTexture*>(texture)) metal->EncodeAlphaForPresentation(presentation);
}
void TVPStopEmoteAsyncAlphaDemand(iTVPTexture2D* texture) {
    if(auto* metal=dynamic_cast<LayerTexture*>(texture)) metal->StopAlphaDemand();
}
uint64_t TVPGetEmoteAlphaPresentationSerial() {
    auto& session=Manager().session;
    auto presentation=session && session->backend ? session->backend->GetCurrentLayerPresentation() : nullptr;
    return presentation ? presentation->frameSerial : 0;
}
std::shared_ptr<krkrsdl3::AsyncLayerPresentation> TVPGetEmoteAlphaPresentation() {
    auto& session=Manager().session;
    return session && session->backend ? session->backend->GetCurrentLayerPresentation() : nullptr;
}
