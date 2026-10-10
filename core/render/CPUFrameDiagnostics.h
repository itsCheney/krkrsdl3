#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>

// Observational, owner-thread-only CPU attribution. Inclusive timings may
// contain nested work and waits; they must not be summed into frame CPU time.
// No script objects, evaluated tags or storage contents are retained.
namespace krkrsdl3::cpu_frame {
enum class Kind { Step, VM, Image, ImageCacheHit, ImageDecode, ScriptStorage,
    KAGLoad, KAGRead, KAGCacheHit, KAGCacheMiss, KAGLabelBuild, KAGNextTag, Count };
inline constexpr const char* names[]={"step","vm","image","image.cacheHit","image.decode",
    "script.storage","kag.load","kag.read","kag.cacheHit","kag.cacheMiss","kag.labelBuild","kag.nextTag"};
struct Metric { uint64_t calls=0,wallNS=0,maxNS=0,failures=0; };
struct Window {
    std::array<Metric,size_t(Kind::Count)> metrics{};
    uint64_t eligible=0,emitted=0,dropped=0,frameDropped=0,windowDropped=0,byteDropped=0;
    uint64_t stackFailures=0,callbackFailures=0,overheadNS=0;
};
inline std::atomic<bool> enabled{false};
inline std::atomic<uint64_t> owner{0},tokens{1};
inline uint64_t ThreadToken() noexcept { static thread_local uint64_t id=tokens.fetch_add(1);return id; }
inline uint64_t Clock() noexcept { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count()); }
inline uint64_t (*clockNS)()=Clock;
inline void (*rawLine)(const char*)=nullptr;
// Return true for a valid (possibly empty) stack, false for unavailable/failure.
// Capture must honor both byte and frame caps and may only inspect the VM owner.
inline bool (*captureStack)(char*,size_t,unsigned)=nullptr;
inline uint64_t epoch=0,windowID=0,nextStep=0,nextCall=0,byteStarted=0,byteUsed=0;
struct Emission {uint64_t time=0,bytes=0;};
inline std::array<Emission,32> emissions{};
inline size_t emissionCount=0;
inline Window window;
inline thread_local uint64_t stepID=0,callID=0;
inline thread_local unsigned frameDetails=0;
inline thread_local bool emitting=false;
inline bool Active() noexcept { return enabled.load(std::memory_order_relaxed) &&
    owner.load(std::memory_order_relaxed)==ThreadToken() && !emitting; }
inline void SetEnabled(bool value,uint64_t newEpoch,uint64_t newWindow) noexcept {
    enabled.store(false,std::memory_order_relaxed);
    owner.store(ThreadToken(),std::memory_order_relaxed);
    epoch=newEpoch;windowID=newWindow;window=Window{};nextStep=nextCall=0;
    stepID=callID=0;frameDetails=0;byteStarted=byteUsed=0;
    emissionCount=0;
    enabled.store(value,std::memory_order_relaxed);
}
inline bool Emit(const char* line) noexcept {
    if(!rawLine) return false;
    emitting=true;
    try {rawLine(line);emitting=false;return true;}
    catch(...) {emitting=false;++window.callbackFailures;return false;}
}
// Escape into a fixed printable field, preserve whole UTF-8 sequences and cap
// the escaped representation too. Native lines never exceed 960 bytes.
inline void Escape(char* out,size_t capacity,const char* in) noexcept {
    size_t n=0;
    for(size_t i=0;in && in[i];) {
        const auto c=static_cast<unsigned char>(in[i]);
        size_t count=c<128 ? 1 : c>=0xc2 && c<=0xdf ? 2 : c>=0xe0 && c<=0xef ? 3 : c>=0xf0 && c<=0xf4 ? 4 : 0;
        if(!count) {++i;continue;}
        bool valid=true;for(size_t j=1;j<count;++j) if(!in[i+j] || (static_cast<unsigned char>(in[i+j])&0xc0)!=0x80) {valid=false;break;}
        if(!valid) {++i;continue;}
        const size_t extra=count==1 && (c=='"' || c=='\\' || c<32) ? 2 : count;
        if(n+extra>=capacity) break;
        if(extra==2 && count==1) {out[n++]='\\';out[n++]=c=='"' ? '"' : c=='\\' ? '\\' : ' ';}
        else {std::memcpy(out+n,in+i,count);n+=count;}
        i+=count;
    }
    out[n]=0;
}
class EventScope {
    Kind kind;
    uint64_t started=0,capturedEpoch=0,capturedStep=0,capturedCall=0,parent=0;
    int exceptions=0;
    bool finished=false;
public:
    explicit EventScope(Kind value,uint64_t parentID=0) noexcept:kind(value) {
        if(!Active()) return;
        started=clockNS();capturedEpoch=epoch;capturedStep=stepID;capturedCall=callID;parent=parentID;
        exceptions=std::uncaught_exceptions();
        window.overheadNS+=clockNS()-started;
    }
    EventScope(const EventScope&)=delete;
    EventScope& operator=(const EventScope&)=delete;
    void Finish(bool failure=false) noexcept {
        if(finished) return;
        finished=true;
        if(!capturedEpoch || !Active() || epoch!=capturedEpoch) return;
        const auto finishedAt=clockNS(),elapsed=finishedAt>=started ? finishedAt-started : 0;
        auto& metric=window.metrics[size_t(kind)];++metric.calls;metric.wallNS+=elapsed;
        metric.maxNS=std::max(metric.maxNS,elapsed);metric.failures+=failure || std::uncaught_exceptions()>exceptions;
        if(elapsed>=16000000) {
            ++window.eligible;
            if(window.emitted>=8) {++window.dropped;++window.windowDropped;}
            else if(frameDetails>=4) {++window.dropped;++window.frameDropped;}
            else {
                char stack[513]{},escaped[513]{},line[961]{};
                // Reserve worst-case line bytes before invoking the tracer.
                while(emissionCount && finishedAt>=emissions[0].time && finishedAt-emissions[0].time>=1000000000) {
                    byteUsed-=emissions[0].bytes;
                    for(size_t i=1;i<emissionCount;++i)emissions[i-1]=emissions[i];
                    --emissionCount;
                }
                if(byteUsed+960>8192 || emissionCount==emissions.size()) {++window.dropped;++window.byteDropped;}
                else {
                    if(captureStack) {try {if(!captureStack(stack,sizeof(stack),4)) ++window.stackFailures;}
                        catch(...) {++window.stackFailures;stack[0]=0;}}
                    else ++window.stackFailures;
                    stack[512]=0;Escape(escaped,sizeof(escaped),stack);
                    const int length=std::snprintf(line,sizeof(line),
                        "cpu.slow v=1 epoch=%llu window=%llu stepID=%llu callID=%llu parentCallID=%llu kind=%s wallNS=%llu failed=%u stack=\"%s\"",
                        (unsigned long long)epoch,(unsigned long long)windowID,(unsigned long long)capturedStep,
                        (unsigned long long)capturedCall,(unsigned long long)parent,names[size_t(kind)],
                        (unsigned long long)elapsed,unsigned(failure || std::uncaught_exceptions()>exceptions),escaped);
                    if(length>0 && size_t(length)<sizeof(line) && Emit(line)) {
                        ++window.emitted;++frameDetails;byteUsed+=size_t(length)+1;
                        emissions[emissionCount++]={finishedAt,uint64_t(length)+1};
                    }
                    else {++window.dropped;}
                }
            }
        }
        window.overheadNS+=clockNS()-finishedAt;
    }
    ~EventScope() noexcept {Finish();}
};
class CallScope {
    uint64_t previous=0,capturedEpoch=0;
    EventScope event;
    static uint64_t Enter(uint64_t& previous,uint64_t& savedEpoch) noexcept {
        if(!Active()) return 0;
        const auto begin=clockNS();
        previous=callID;savedEpoch=epoch;callID=++nextCall;
        window.overheadNS+=clockNS()-begin;
        return previous;
    }
public:
    CallScope() noexcept:event(Kind::VM,Enter(previous,capturedEpoch)) {}
    void Finish(bool failed=false) noexcept {event.Finish(failed);}
    ~CallScope() noexcept {event.Finish();if(capturedEpoch && epoch==capturedEpoch) callID=previous;}
};
class StepScope {
    uint64_t previousStep=0,previousCall=0,capturedEpoch=0;
    unsigned previousDetails=0;
    EventScope event;
    Kind Enter() noexcept {
        if(Active()) {
            const auto begin=clockNS();
            capturedEpoch=epoch;previousStep=stepID;previousCall=callID;previousDetails=frameDetails;
            stepID=++nextStep;callID=0;frameDetails=0;
            window.overheadNS+=clockNS()-begin;
        }
        return Kind::Step;
    }
public:
    StepScope() noexcept:event(Enter()) {}
    ~StepScope() noexcept {event.Finish();if(capturedEpoch && epoch==capturedEpoch) {
        stepID=previousStep;callID=previousCall;frameDetails=previousDetails;}}
};
inline void TakeWindow(uint64_t takenID,uint64_t nextID) noexcept {
    if(!Active()) return;
    const auto begin=clockNS();
    const auto captured=window;window=Window{};windowID=nextID;
    char line[961]{};
    std::snprintf(line,sizeof(line),"cpu.summary v=1 epoch=%llu window=%llu eligible=%llu emitted=%llu dropped=%llu frameDropped=%llu windowDropped=%llu byteDropped=%llu stackFailures=%llu callbackFailures=%llu overheadNS=%llu",
        (unsigned long long)epoch,(unsigned long long)takenID,(unsigned long long)captured.eligible,
        (unsigned long long)captured.emitted,(unsigned long long)captured.dropped,(unsigned long long)captured.frameDropped,
        (unsigned long long)captured.windowDropped,(unsigned long long)captured.byteDropped,
        (unsigned long long)captured.stackFailures,(unsigned long long)captured.callbackFailures,(unsigned long long)captured.overheadNS);
    Emit(line);
    for(size_t i=0;i<captured.metrics.size();++i) {
        const auto& m=captured.metrics[i];
        std::snprintf(line,sizeof(line),"cpu.aggregate v=1 epoch=%llu window=%llu kind=%s calls=%llu wallNS=%llu maxNS=%llu failures=%llu",
            (unsigned long long)epoch,(unsigned long long)takenID,names[i],(unsigned long long)m.calls,
            (unsigned long long)m.wallNS,(unsigned long long)m.maxNS,(unsigned long long)m.failures);
        Emit(line);
    }
    // Reporting belongs to the following window, whose summary can include
    // its complete elapsed cost without recursively logging another summary.
    window.overheadNS+=clockNS()-begin;
}
}
