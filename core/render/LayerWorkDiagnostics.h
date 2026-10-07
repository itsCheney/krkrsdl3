#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

// Bounded, opt-in diagnostics. Timings are inclusive wall time: script/load/
// software scopes can contain each other and GPU waits, so never add them.
namespace krkrsdl3::layer_work {
inline std::atomic<bool> enabled{false};
inline std::atomic<uint64_t> generation{0};
inline std::atomic<uint64_t> nextThread{1}, timingThread{0};
inline uint64_t ThreadToken() {
    static thread_local const uint64_t token=nextThread.fetch_add(1,std::memory_order_relaxed);
    return token;
}
inline thread_local const char* source="unattributed";
inline uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
class SourceScope {
    const char* previous;
public:
    explicit SourceScope(const char* name):previous(source) { source=name && *name ? name : "unattributed"; }
    SourceScope(const SourceScope&)=delete;
    SourceScope& operator=(const SourceScope&)=delete;
    ~SourceScope() { source=previous; }
};
enum class Stage { ResourceLoad, AMVDecode, Script, GC, Compact, Software,
    ImageLoad, ImageDecode, ImageCacheHit, ScriptStorage, ImageResolve, ImageOpen, ImageCodec, Count };
struct Timing { uint64_t calls=0, ns=0, maxNS=0; };
struct Transfer {
    bool upload=false, leased=false;
    uint64_t texture=0, calls=0, bytes=0, ns=0, waitNS=0;
    int width=0,height=0;
    char source[48]{};
};
inline constexpr size_t OriginCapacity=64, MaxFrameSamples=2048;
inline constexpr uint64_t RecordEpochSentinel=std::numeric_limits<uint64_t>::max();
inline constexpr std::array<const char*,20> protectedOrigins={
    "transition.source","transition.outputOverwrite","transition.output","transition.outputUnscoped",
    "shrinkCopy.read","shrinkCopy.write","layerExBase","layerExBase.write","layerExDraw.write",
    "bitmap.update","bitmap.lock","bitmap.scanline","bitmap.cpuWrite","bitmap.point",
    "script.rawPointer","script.rawWrite","fallback","session.detach","mixed","unattributed"};
static_assert(OriginCapacity*(7+47*3+1+20*4+3+1)<16384,"Origin rows must fit the C bridge without truncation");
struct OriginTransfer {
    bool upload=false;
    uint64_t calls=0,bytes=0,ns=0,waitNS=0;
    char source[48]{};
};
// Each row is a detail view of the existing transfer counters, never an
// additional transfer. Strings are rejected on overflow rather than merged.
inline constexpr size_t TransitionCapacity=64, TransitionJSONCapacity=131072;
struct TransitionMetrics {
    uint64_t frames=0,gpuCalls=0,cpuCalls=0,passthroughCalls=0,pixels=0;
    uint64_t parameterUploads=0,parameterBytes=0;
    uint64_t readCalls=0,readBytes=0,readWallNS=0,readWaitNS=0;
    uint64_t uploadCalls=0,uploadBytes=0,uploadWallNS=0,uploadWaitNS=0;
};
struct TransitionRecord {
    char requested[96]{},effective[96]{},reason[64]{},metadata[256]{};
    int dimensions[8]{};
    uint64_t firstTick=0,lastTick=0,lastFrame=0;
    bool haveFrame=false;
    TransitionMetrics metrics;
};
// String escaped lengths are checked independently below. Numeric fields are
// bounded by decimal int32 (11 bytes) and uint64 (20 bytes); 480 bytes covers
// every JSON key, punctuation and boolean in a row.
static_assert(TransitionCapacity*(2*191+127+511+8*11+19*20+480)+2<TransitionJSONCapacity,
              "All bounded transition rows must fit the C bridge");
inline std::string JSONString(const char* value) {
    constexpr char hex[]="0123456789abcdef";
    std::string out="\"";
    for(const auto* c=reinterpret_cast<const unsigned char*>(value);*c;++c) {
        if(*c=='"' || *c=='\\') {out+='\\';out+=char(*c);}
        else if(*c<32) {out+="\\u00";out+=hex[*c>>4];out+=hex[*c&15];}
        else out+=char(*c);
    }
    return out+'"';
}
inline bool TransitionString(char* dest,size_t capacity,const char* value,size_t escapedLimit) {
    value=value ? value : "";
    size_t size=0,escaped=0;
    for(const auto* c=reinterpret_cast<const unsigned char*>(value);*c;++c) {
        ++size;escaped+=*c<32 ? 6 : (*c=='"' || *c=='\\') ? 2 : 1;
        if(size>=capacity || escaped>escapedLimit) return false;
    }
    std::memcpy(dest,value,size+1);return true;
}
struct Profile { std::array<Timing,size_t(Stage::Count)> stages{};
    std::array<Transfer,64> transfers{}; size_t size=0;
    Transfer overflow[2]{};
    std::array<OriginTransfer,OriginCapacity> origins{}; size_t originSize=protectedOrigins.size()*2;
    OriginTransfer originOverflow[2]{};
    uint64_t capacityRecords=0,oversizeRecords=0;
    uint32_t frameSampleCount=0;
    std::array<uint64_t,MaxFrameSamples> frameIntervalNS{},frameCpuWallNS{};
    uint64_t frameSamplesDropped=0;
    uint64_t decodedFrames=0, decodedBytes=0;
    std::array<TransitionRecord,TransitionCapacity> transitions{};size_t transitionSize=0;
    TransitionMetrics transitionOverflow;
    uint64_t transitionProfilesDropped=0,transitionCapacityRecords=0,transitionOversizeRecords=0;
    Profile() {
        for(size_t i=0;i<protectedOrigins.size();++i) for(size_t direction=0;direction<2;++direction) {
            auto& origin=origins[i*2+direction]; origin.upload=direction!=0;
            std::memcpy(origin.source,protectedOrigins[i],std::strlen(protectedOrigins[i])+1);
        }
    }
};
inline Profile profile;
inline uint64_t started=0;
inline std::mutex mutex;
inline uint64_t lastFrameTimestamp=0;
inline bool haveLastFrameTimestamp=false;
inline thread_local unsigned depth[size_t(Stage::Count)]{};
inline void SetEnabled(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    enabled.store(value,std::memory_order_relaxed);
    timingThread.store(value ? ThreadToken() : 0,std::memory_order_relaxed);
    auto next=generation.load(std::memory_order_relaxed)+1;
    if(next==0 || next==RecordEpochSentinel) next=1;
    generation.store(next,std::memory_order_relaxed);
    profile=Profile{}; started=value ? Now() : 0;
    lastFrameTimestamp=0; haveLastFrameTimestamp=false;
}
inline uint64_t CaptureGeneration() {
    if(!enabled.load(std::memory_order_relaxed)) return 0;
    std::lock_guard<std::mutex> lock(mutex);
    return enabled.load(std::memory_order_relaxed) ? generation.load(std::memory_order_relaxed) : 0;
}
class TransitionScope;
inline thread_local TransitionScope* transitionScope=nullptr;
inline std::atomic<uint64_t> nextTransitionFrame{1};
inline void AddTransitionMetrics(TransitionMetrics& to,const TransitionMetrics& from) {
    to.frames+=from.frames;to.gpuCalls+=from.gpuCalls;to.cpuCalls+=from.cpuCalls;
    to.passthroughCalls+=from.passthroughCalls;to.pixels+=from.pixels;
    to.parameterUploads+=from.parameterUploads;to.parameterBytes+=from.parameterBytes;
    to.readCalls+=from.readCalls;to.readBytes+=from.readBytes;to.readWallNS+=from.readWallNS;to.readWaitNS+=from.readWaitNS;
    to.uploadCalls+=from.uploadCalls;to.uploadBytes+=from.uploadBytes;to.uploadWallNS+=from.uploadWallNS;to.uploadWaitNS+=from.uploadWaitNS;
}
class TransitionScope {
    TransitionScope* previous=nullptr;
    TransitionRecord record;
    uint64_t epoch=0,frame=0,pixels=0;
    bool valid=true,result=false,routeGPU=false,routeCPU=false,passthrough=false;
public:
    TransitionScope(const char* requested,const char* effective,int canvasW,int canvasH,
        int src1W,int src1H,int src2W,int src2H,int outputW,int outputH,
        uint64_t tick=0,uint64_t frameID=0,const char* metadata="{}") {
        // Even an inactive nested scope masks its parent. An enable operation
        // during this call cannot retroactively attach work to an older scope.
        previous=transitionScope;transitionScope=this;
        epoch=CaptureGeneration();frame=frameID ? frameID : tick;
        if(!epoch) return;
        valid=TransitionString(record.requested,sizeof(record.requested),requested,191) &
              TransitionString(record.effective,sizeof(record.effective),effective,191) &
              TransitionString(record.metadata,sizeof(record.metadata),metadata,511);
        const int dimensions[]={canvasW,canvasH,src1W,src1H,src2W,src2H,outputW,outputH};
        std::copy_n(dimensions,8,record.dimensions);record.firstTick=record.lastTick=tick;
        std::strcpy(record.reason,"provider.cpu");
    }
    TransitionScope(const TransitionScope&)=delete;
    TransitionScope& operator=(const TransitionScope&)=delete;
    void SetMetadata(const char* metadata) {
        if(epoch) valid=TransitionString(record.metadata,sizeof(record.metadata),metadata,511) && valid;
    }
    void SetPixels(uint64_t count) {pixels=count;}
    void Result(bool gpu,const char* reason,uint64_t count,uint64_t parameterUploads=0,uint64_t parameterBytes=0) {
        if(!epoch) return;
        result=true;passthrough=reason && !std::strcmp(reason,"passthrough");
        if(gpu) routeGPU=true;else if(!passthrough) routeCPU=true;
        // One Process can contain several render-manager operations (scroll).
        // CPU wins for mixed execution; retain the concrete fallback reason.
        if(!gpu || !routeCPU) valid=TransitionString(record.reason,sizeof(record.reason),reason,127) && valid;
        record.metrics.pixels+=count;record.metrics.parameterUploads+=parameterUploads;record.metrics.parameterBytes+=parameterBytes;
    }
    void Parameters(uint64_t calls,uint64_t bytes) {
        if(epoch) {record.metrics.parameterUploads+=calls;record.metrics.parameterBytes+=bytes;}
    }
    void Transfer(bool upload,uint64_t bytes,uint64_t ns,uint64_t waitNS,uint64_t transferEpoch) {
        if(!epoch || epoch!=transferEpoch) return;
        auto& m=record.metrics;
        if(upload) {++m.uploadCalls;m.uploadBytes+=bytes;m.uploadWallNS+=ns;m.uploadWaitNS+=waitNS;}
        else {++m.readCalls;m.readBytes+=bytes;m.readWallNS+=ns;m.readWaitNS+=waitNS;}
    }
    ~TransitionScope() {
        transitionScope=previous;
        if(!epoch) return;
        if(!result || routeCPU) record.metrics.cpuCalls=1;
        else if(routeGPU) record.metrics.gpuCalls=1;
        else if(passthrough) record.metrics.passthroughCalls=1;
        if(routeGPU && routeCPU) {
            char mixed[sizeof(record.reason)+6]{};
            std::snprintf(mixed,sizeof(mixed),"mixed.%s",record.reason);
            valid=TransitionString(record.reason,sizeof(record.reason),mixed,127) && valid;
        }
        if(pixels) record.metrics.pixels=pixels;
        std::lock_guard<std::mutex> lock(mutex);
        if(!enabled.load(std::memory_order_relaxed) || epoch!=generation.load(std::memory_order_relaxed)) return;
        TransitionRecord* found=nullptr;
        if(valid) {
            for(size_t i=0;i<profile.transitionSize;++i) {
                auto& t=profile.transitions[i];
                if(!std::strcmp(t.requested,record.requested) && !std::strcmp(t.effective,record.effective) &&
                   !std::strcmp(t.reason,record.reason) && !std::strcmp(t.metadata,record.metadata) &&
                   std::equal(t.dimensions,t.dimensions+8,record.dimensions)) {found=&t;break;}
            }
            if(!found && profile.transitionSize<TransitionCapacity) {
                found=&profile.transitions[profile.transitionSize++];*found=record;found->metrics={};
            }
        }
        if(found) {
            record.metrics.frames=(!found->haveFrame || found->lastFrame!=frame) ? 1 : 0;
            found->haveFrame=true;found->lastFrame=frame;found->lastTick=record.lastTick;
            AddTransitionMetrics(found->metrics,record.metrics);
        } else {
            ++profile.transitionProfilesDropped;
            if(valid) ++profile.transitionCapacityRecords;else ++profile.transitionOversizeRecords;
            record.metrics.frames=1;AddTransitionMetrics(profile.transitionOverflow,record.metrics);
        }
    }
};
inline void SetTransitionMetadata(const char* metadata) {if(transitionScope) transitionScope->SetMetadata(metadata);}
inline void RecordTransitionResult(bool gpu,const char* reason,uint64_t pixels,uint64_t calls=0,uint64_t bytes=0) {
    if(transitionScope) transitionScope->Result(gpu,reason,pixels,calls,bytes);
}
inline void RecordTransitionParameters(uint64_t calls,uint64_t bytes) {if(transitionScope) transitionScope->Parameters(calls,bytes);}
inline void ResetFrameHistory() {
    std::lock_guard<std::mutex> lock(mutex);
    lastFrameTimestamp=0; haveLastFrameTimestamp=false;
    profile.frameSampleCount=0; profile.frameSamplesDropped=0;
    profile.frameIntervalNS.fill(0); profile.frameCpuWallNS.fill(0);
}
class StageScope {
    Stage stage; bool active=false, outer=false; uint64_t started=0, epoch=0;
public:
    explicit StageScope(Stage value):stage(value) {
        active=enabled.load(std::memory_order_relaxed) && timingThread.load(std::memory_order_relaxed)==ThreadToken();
        if(active) { epoch=generation.load(std::memory_order_relaxed); outer=depth[size_t(stage)]++==0; if(outer) started=Now(); }
    }
    StageScope(const StageScope&)=delete;
    StageScope& operator=(const StageScope&)=delete;
    ~StageScope() {
        if(!active) return;
        --depth[size_t(stage)];
        if(!outer || !enabled.load(std::memory_order_relaxed) || epoch!=generation.load(std::memory_order_relaxed)) return;
        const uint64_t elapsed=Now()-started;
        std::lock_guard<std::mutex> lock(mutex);
        if(!enabled.load(std::memory_order_relaxed) || epoch!=generation.load(std::memory_order_relaxed)) return;
        auto& t=profile.stages[size_t(stage)]; ++t.calls; t.ns+=elapsed; t.maxNS=std::max(t.maxNS,elapsed);
    }
};
inline void Record(bool upload,uint64_t texture,int width,int height,uint64_t bytes,
                   uint64_t ns=0,uint64_t waitNS=0,bool leased=false,const char* origin=nullptr,
                   uint64_t epoch=RecordEpochSentinel,bool oversizeOrigin=false) {
    if(epoch==RecordEpochSentinel) epoch=CaptureGeneration();
    if(!epoch) return;
    const char* rawName=origin ? origin : source;
    if(!*rawName) rawName="unattributed";
    const size_t length=std::strlen(rawName);
    // Preserve the legacy texture view's 47-byte display name. The origin
    // view separately rejects oversized names rather than merging prefixes.
    char name[48]{};std::memcpy(name,rawName,std::min(length,sizeof(name)-1));
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled.load(std::memory_order_relaxed) || epoch!=generation.load(std::memory_order_relaxed)) return;
    if(transitionScope) transitionScope->Transfer(upload,bytes,ns,waitNS,epoch);
    Transfer* found=nullptr;
    for(size_t i=0;i<profile.size;++i) {
        auto& t=profile.transfers[i];
        if(t.upload==upload && t.texture==texture && t.leased==leased && !std::strcmp(t.source,name)) { found=&t; break; }
    }
    if(!found && profile.size<profile.transfers.size()) {
        found=&profile.transfers[profile.size++]; found->upload=upload; found->texture=texture;
        found->width=width; found->height=height; found->leased=leased;
        std::memcpy(found->source,name,sizeof(name));
    }
    if(!found) found=&profile.overflow[upload ? 1 : 0];
    ++found->calls; found->bytes+=bytes; found->ns+=ns; found->waitNS+=waitNS;
    OriginTransfer* group=nullptr;
    if(oversizeOrigin || length>=sizeof(OriginTransfer::source)) ++profile.oversizeRecords;
    else {
        for(size_t i=0;i<profile.originSize;++i) {
            auto& value=profile.origins[i];
            if(value.upload==upload && !std::strcmp(value.source,name)) {group=&value;break;}
        }
        if(!group && profile.originSize<profile.origins.size()) {
            group=&profile.origins[profile.originSize++];group->upload=upload;
            std::memcpy(group->source,name,length+1);
        }
        if(!group) ++profile.capacityRecords;
    }
    if(!group) group=&profile.originOverflow[upload ? 1 : 0];
    ++group->calls;group->bytes+=bytes;group->ns+=ns;group->waitNS+=waitNS;
}
inline void RecordFrame(uint64_t presentedAt,uint64_t workNS) {
    if(!enabled.load(std::memory_order_relaxed)) return;
    const auto thread=ThreadToken();
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled.load(std::memory_order_relaxed) || timingThread.load(std::memory_order_relaxed)!=thread) return;
    const auto interval=haveLastFrameTimestamp && presentedAt>=lastFrameTimestamp ? presentedAt-lastFrameTimestamp : 0;
    lastFrameTimestamp=presentedAt;haveLastFrameTimestamp=true;
    if(profile.frameSampleCount>=MaxFrameSamples) {++profile.frameSamplesDropped;return;}
    const auto index=profile.frameSampleCount++;
    profile.frameIntervalNS[index]=interval;profile.frameCpuWallNS[index]=workNS;
}
inline void RecordAMVFrame(uint64_t bytes) {
    if(!enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled.load(std::memory_order_relaxed)) return;
    ++profile.decodedFrames; profile.decodedBytes+=bytes;
}
struct Summary {
    std::string stages,transfers,transferOrigins,originOverflow;
    uint32_t workProfileVersion=2,frameSampleCount=0;
    std::array<uint64_t,MaxFrameSamples> frameIntervalNS{},frameCpuWallNS{};
    uint64_t intervalNS=0,decodedFrames=0,decodedBytes=0,frameSamplesDropped=0;
    uint32_t transitionProfileVersion=1;
    std::string transitionProfiles="[]",transitionOverflow;
    uint64_t transitionProfilesDropped=0;
};
inline std::string TransitionMetricsJSON(const TransitionMetrics& m) {
    std::string out;
    const auto append=[&](const char* key,uint64_t value) {
        if(!out.empty()) out+=',';
        out+=JSONString(key)+':'+std::to_string(value);
    };
    append("frames",m.frames);append("gpuCalls",m.gpuCalls);append("cpuCalls",m.cpuCalls);
    append("passthroughCalls",m.passthroughCalls);append("pixels",m.pixels);
    append("parameterUploads",m.parameterUploads);append("parameterBytes",m.parameterBytes);
    append("readCalls",m.readCalls);append("readBytes",m.readBytes);append("readWallNS",m.readWallNS);append("readWaitNS",m.readWaitNS);
    append("uploadCalls",m.uploadCalls);append("uploadBytes",m.uploadBytes);append("uploadWallNS",m.uploadWallNS);append("uploadWaitNS",m.uploadWaitNS);
    return out;
}
inline std::string EncodeOrigin(const char* name) {
    constexpr char hex[]="0123456789ABCDEF";
    std::string result;
    for(const auto* byte=reinterpret_cast<const unsigned char*>(name);*byte;++byte) {
        const auto c=*byte;
        if((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='.' || c=='_' || c=='-') result+=char(c);
        else {result+='%';result+=hex[c>>4];result+=hex[c&15];}
    }
    return result;
}
inline std::string OriginMetrics(const OriginTransfer& value) {
    return std::to_string(value.calls)+"/"+std::to_string(value.bytes)+"/"+std::to_string(value.ns)+"/"+std::to_string(value.waitNS);
}
inline Summary Take() {
    Profile captured;
    Summary out;
    { std::lock_guard<std::mutex> lock(mutex); captured=profile; profile=Profile{};
      const auto now=Now(); out.intervalNS=started && now>=started ? now-started : 0;
      started=enabled.load(std::memory_order_relaxed) ? now : 0; }
    out.decodedFrames=captured.decodedFrames; out.decodedBytes=captured.decodedBytes;
    out.frameSampleCount=captured.frameSampleCount;out.frameSamplesDropped=captured.frameSamplesDropped;
    out.frameIntervalNS=captured.frameIntervalNS;out.frameCpuWallNS=captured.frameCpuWallNS;
    out.transitionProfilesDropped=captured.transitionProfilesDropped;
    out.transitionProfiles="[";
    for(size_t i=0;i<captured.transitionSize;++i) {
        const auto& t=captured.transitions[i];if(i) out.transitionProfiles+=',';
        out.transitionProfiles+="{\"requested\":"+JSONString(t.requested)+",\"effective\":"+JSONString(t.effective)+
            ",\"reason\":"+JSONString(t.reason)+",\"metadata\":"+JSONString(t.metadata)+",\"dimensions\":[";
        for(int d=0;d<8;++d) {if(d) out.transitionProfiles+=',';out.transitionProfiles+=std::to_string(t.dimensions[d]);}
        out.transitionProfiles+="],\"firstTick\":"+std::to_string(t.firstTick)+",\"lastTick\":"+std::to_string(t.lastTick)+','+
            TransitionMetricsJSON(t.metrics)+'}';
    }
    out.transitionProfiles+=']';
    out.transitionOverflow="{"+TransitionMetricsJSON(captured.transitionOverflow)+",\"capacityRecords\":"+
        std::to_string(captured.transitionCapacityRecords)+",\"oversizeRecords\":"+std::to_string(captured.transitionOversizeRecords)+'}';
    std::sort(captured.origins.begin(),captured.origins.begin()+captured.originSize,
        [](const OriginTransfer& a,const OriginTransfer& b) {
            if(a.waitNS!=b.waitNS) return a.waitNS>b.waitNS;
            if(a.bytes!=b.bytes) return a.bytes>b.bytes;
            const int order=std::strcmp(a.source,b.source);
            return order ? order<0 : a.upload<b.upload;
        });
    for(size_t i=0;i<captured.originSize;++i) {
        const auto& origin=captured.origins[i];if(!origin.calls) continue;
        if(!out.transferOrigins.empty()) out.transferOrigins+=',';
        out.transferOrigins+=(origin.upload ? "upload:" : "read:")+EncodeOrigin(origin.source)+"="+OriginMetrics(origin);
    }
    out.originOverflow="read="+OriginMetrics(captured.originOverflow[0])+",upload="+OriginMetrics(captured.originOverflow[1])+
        ",capacityRecords="+std::to_string(captured.capacityRecords)+",oversizeRecords="+std::to_string(captured.oversizeRecords);
    constexpr const char* names[]={"resourceLoad","amvDecode","script","gc","compact","software",
        "imageLoad","imageDecode","imageCacheHit","scriptStorage","imageResolve","imageOpen","imageCodec"};
    for(size_t i=0;i<captured.stages.size();++i) {
        const auto& t=captured.stages[i];
        if(i) out.stages+=',';
        out.stages+=std::string(names[i])+":"+std::to_string(t.calls)+"/"+std::to_string(t.ns)+"/"+std::to_string(t.maxNS);
    }
    std::sort(captured.transfers.begin(),captured.transfers.begin()+captured.size,
        [](const Transfer& a,const Transfer& b) { return a.bytes>b.bytes; });
    for(size_t i=0;i<captured.size;++i) {
        const auto& t=captured.transfers[i];
        if(i>=8) {
            auto& other=captured.overflow[t.upload ? 1 : 0];
            other.calls+=t.calls; other.bytes+=t.bytes; other.ns+=t.ns; other.waitNS+=t.waitNS; continue;
        }
        if(!out.transfers.empty()) out.transfers+=',';
        out.transfers+=(t.upload ? "upload:" : "read:")+std::string(t.source)+"@"+std::to_string(t.texture)+
            "("+std::to_string(t.width)+"x"+std::to_string(t.height)+",lease="+std::to_string(t.leased)+")="+
            std::to_string(t.calls)+"/"+std::to_string(t.bytes)+"/"+std::to_string(t.ns)+"/"+std::to_string(t.waitNS);
    }
    for(int i=0;i<2;++i) {
        const auto& t=captured.overflow[i]; if(!t.calls) continue;
        if(!out.transfers.empty()) out.transfers+=',';
        out.transfers+=(i ? "upload:other=" : "read:other=")+std::to_string(t.calls)+"/"+std::to_string(t.bytes)+
            "/"+std::to_string(t.ns)+"/"+std::to_string(t.waitNS);
    }
    return out;
}
}
