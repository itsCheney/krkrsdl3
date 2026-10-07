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
};
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
