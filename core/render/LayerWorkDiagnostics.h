#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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
    explicit SourceScope(const char* name):previous(source) { source=name ? name : "unattributed"; }
    SourceScope(const SourceScope&)=delete;
    SourceScope& operator=(const SourceScope&)=delete;
    ~SourceScope() { source=previous; }
};
enum class Stage { ResourceLoad, AMVDecode, Script, GC, Compact, Software,
    ImageLoad, ImageDecode, ImageCacheHit, ScriptStorage, Count };
struct Timing { uint64_t calls=0, ns=0, maxNS=0; };
struct Transfer {
    bool upload=false, leased=false;
    uint64_t texture=0, calls=0, bytes=0, ns=0, waitNS=0;
    int width=0,height=0;
    char source[48]{};
};
struct Profile { std::array<Timing,size_t(Stage::Count)> stages{};
    std::array<Transfer,64> transfers{}; size_t size=0;
    Transfer overflow[2]{};
    uint64_t decodedFrames=0, decodedBytes=0;
};
inline Profile profile;
inline uint64_t started=0;
inline std::mutex mutex;
inline thread_local unsigned depth[size_t(Stage::Count)]{};
inline void SetEnabled(bool value) {
    enabled.store(value,std::memory_order_relaxed);
    timingThread.store(value ? ThreadToken() : 0,std::memory_order_relaxed);
    generation.fetch_add(1,std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex); profile=Profile{}; started=value ? Now() : 0;
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
                   uint64_t ns=0,uint64_t waitNS=0,bool leased=false,const char* origin=nullptr) {
    if(!enabled.load(std::memory_order_relaxed)) return;
    char name[48]{}; std::snprintf(name,sizeof(name),"%s",origin ? origin : source);
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled.load(std::memory_order_relaxed)) return;
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
}
inline void RecordAMVFrame(uint64_t bytes) {
    if(!enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled.load(std::memory_order_relaxed)) return;
    ++profile.decodedFrames; profile.decodedBytes+=bytes;
}
struct Summary { std::string stages, transfers; uint64_t intervalNS=0, decodedFrames=0, decodedBytes=0; };
inline Summary Take() {
    Profile captured;
    Summary out;
    { std::lock_guard<std::mutex> lock(mutex); captured=profile; profile=Profile{};
      const auto now=Now(); out.intervalNS=started ? now-started : 0; started=now; }
    out.decodedFrames=captured.decodedFrames; out.decodedBytes=captured.decodedBytes;
    constexpr const char* names[]={"resourceLoad","amvDecode","script","gc","compact","software",
        "imageLoad","imageDecode","imageCacheHit","scriptStorage"};
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
