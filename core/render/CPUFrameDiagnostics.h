#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>

// Inclusive wall time contains nested work and waits. Residual wall time only
// subtracts instrumented direct children; it is not pure VM CPU time.
namespace krkrsdl3::cpu_frame {
enum class Kind { Step, VM, Image, ImageCacheHit, ImageDecode, ScriptStorage,
    KAGLoad, KAGRead, KAGCacheHit, KAGCacheMiss, KAGLabelBuild, KAGNextTag,
    PSBLoad, PSBOpen, PSBRead, PSBDecompress, PSBCopy, PSBTables,
    PSBMaterialize, PSBInvalidate, PSBDestroy, Count };
inline constexpr const char* names[]={"step","vm","image","image.cacheHit","image.decode",
    "script.storage","kag.load","kag.read","kag.cacheHit","kag.cacheMiss","kag.labelBuild","kag.nextTag",
    "psb.load","psb.open","psb.read","psb.decompress","psb.copy","psb.tables",
    "psb.materialize","psb.invalidate","psb.destroy"};
static_assert(sizeof(names)/sizeof(*names)==size_t(Kind::Count),"CPU diagnostic kind names must match metrics");
struct Metric { uint64_t calls=0,wallNS=0,maxNS=0,failures=0,workBytes=0; };
struct Window {
    std::array<Metric,size_t(Kind::Count)> metrics{};
    uint64_t eligible=0,emitted=0,dropped=0,frameDropped=0,windowDropped=0,byteDropped=0;
    uint64_t formatDropped=0,callbackDropped=0,stackFailures=0,callbackFailures=0,overheadNS=0;
};
inline std::atomic<bool> enabled{false};
inline std::atomic<uint64_t> owner{0},tokens{1};
inline uint64_t ThreadToken() noexcept { static thread_local uint64_t id=tokens.fetch_add(1);return id; }
inline uint64_t Clock() noexcept { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count()); }
inline uint64_t (*clockNS)()=Clock;
inline void (*rawLine)(const char*)=nullptr;
// Owner-thread-only stack capture, capped at 512 UTF-8 bytes and four frames.
inline bool (*captureStack)(char*,size_t,unsigned)=nullptr;
inline uint64_t epoch=0,windowID=0,nextStep=0,nextCall=0,nextScope=0,byteUsed=0;
struct Emission {uint64_t time=0,bytes=0;};
inline std::array<Emission,32> emissions{};
inline size_t emissionCount=0;
inline Window window;
inline thread_local uint64_t stepID=0,callID=0;
inline thread_local bool emitting=false;
class EventScope;
inline thread_local EventScope* activeScope=nullptr;

// Two heaviest steps, four roles each: step, outer VM, native PSB phase, other
// child. Copy scalars/UTF-8 before unwind; never retain VM objects or resources.
struct Detail {
    bool used=false;
    Kind kind=Kind::Step;
    unsigned role=0;
    uint64_t step=0,call=0,parent=0,scope=0,parentScope=0,startWindow=0;
    uint64_t wallNS=0,childWallNS=0,bytes=0,vmCalls=0;
    bool failed=false;
    char stack[513]{};
};
struct StepCandidates {
    bool used=false;
    uint64_t step=0,weight=0;
    std::array<Detail,4> details{};
    size_t Count() const noexcept {size_t n=0;for(const auto& d:details)n+=d.used;return n;}
};
inline std::array<StepCandidates,2> candidates{};
struct FrameCandidates {StepCandidates group;FrameCandidates* previous=nullptr;};
inline thread_local FrameCandidates* activeFrame=nullptr;
inline StepCandidates outsideCandidates{};
inline bool Active() noexcept { return enabled.load(std::memory_order_relaxed) &&
    owner.load(std::memory_order_relaxed)==ThreadToken() && !emitting; }
inline void SetEnabled(bool value,uint64_t newEpoch,uint64_t newWindow) noexcept {
    enabled.store(false,std::memory_order_relaxed);
    owner.store(ThreadToken(),std::memory_order_relaxed);
    epoch=newEpoch;windowID=newWindow;window=Window{};nextStep=nextCall=nextScope=0;
    stepID=callID=0;activeScope=nullptr;activeFrame=nullptr;byteUsed=0;emissionCount=0;candidates={};outsideCandidates={};
    enabled.store(value,std::memory_order_relaxed);
}
inline bool Emit(const char* line) noexcept {
    if(!rawLine) return false;
    const uint64_t savedEpoch=epoch;
    emitting=true;
    try {rawLine(line);emitting=false;return true;}
    catch(...) {emitting=false;if(epoch==savedEpoch)++window.callbackFailures;return false;}
}
// Preserve complete UTF-8 sequences. False means truncation/invalid UTF-8.
inline bool Escape(char* out,size_t capacity,const char* in) noexcept {
    size_t n=0;bool complete=true;
    for(size_t i=0;in && in[i];) {
        const auto c=static_cast<unsigned char>(in[i]);
        size_t count=c<128 ? 1 : c>=0xc2 && c<=0xdf ? 2 : c>=0xe0 && c<=0xef ? 3 : c>=0xf0 && c<=0xf4 ? 4 : 0;
        if(!count) {++i;complete=false;continue;}
        bool valid=true;for(size_t j=1;j<count;++j) if(!in[i+j] || (static_cast<unsigned char>(in[i+j])&0xc0)!=0x80) {valid=false;break;}
        if(!valid) {++i;complete=false;continue;}
        const size_t extra=count==1 && (c=='"' || c=='\\' || c<32) ? 2 : count;
        if(n+extra>=capacity) {complete=false;break;}
        if(extra==2 && count==1) {out[n++]='\\';out[n++]=c=='"' ? '"' : c=='\\' ? '\\' : ' ';}
        else {std::memcpy(out+n,in+i,count);n+=count;}
        i+=count;
    }
    out[n]=0;return complete;
}
inline void Drop(uint64_t& reason,uint64_t count=1) noexcept {window.dropped+=count;reason+=count;}
inline void Merge(const StepCandidates& value) noexcept {
    if(!value.Count())return;
    StepCandidates* group=nullptr;
    for(auto& c:candidates)if(c.used && c.step==value.step)group=&c;
    if(!group) {
        for(auto& c:candidates)if(!c.used) {group=&c;break;}
        if(!group) {
            auto& lightest=*std::min_element(candidates.begin(),candidates.end(),
                [](const auto& a,const auto& b){return a.weight<b.weight;});
            if(value.weight<=lightest.weight) {Drop(window.windowDropped,value.Count());return;}
            Drop(window.windowDropped,lightest.Count());group=&lightest;
        }
        *group=StepCandidates{};group->used=true;group->step=value.step;
    }
    group->weight=std::max(group->weight,value.weight);
    for(const auto& detail:value.details)if(detail.used) {
        auto& slot=group->details[detail.role];
        if(slot.used) {
            Drop(window.frameDropped);
            if(slot.wallNS>=detail.wallNS)continue;
        }
        slot=detail;
    }
}
inline void Select(Detail value) noexcept {
    auto* group=activeFrame && activeFrame->group.step==value.step ? &activeFrame->group : &outsideCandidates;
    if(group->used && group->step!=value.step) {Merge(*group);*group=StepCandidates{};}
    group->used=true;group->step=value.step;group->weight=std::max(group->weight,value.wallNS);
    auto& slot=group->details[value.role];
    if(slot.used) {
        if(value.wallNS<=slot.wallNS) {Drop(window.frameDropped);return;}
        Drop(window.frameDropped);
    }
    const uint64_t savedEpoch=epoch;
    bool unavailable=false;
    emitting=true;
    if(captureStack) {
        try {unavailable=!captureStack(value.stack,sizeof(value.stack),4);}
        catch(...) {unavailable=true;value.stack[0]=0;}
    } else unavailable=true;
    emitting=false;value.stack[512]=0;
    if(epoch!=savedEpoch || !Active()) return;
    if(unavailable)++window.stackFailures;
    value.used=true;slot=value;
}

class EventScope {
    Kind kind;
    uint64_t started=0,capturedEpoch=0,capturedStep=0,capturedCall=0,parent=0;
    uint64_t scope=0,parentScope=0,startWindow=0,childNS=0,workBytes=0,startedCall=0;
    EventScope* previous=nullptr;
    int exceptions=0;
    bool finished=false;
public:
    explicit EventScope(Kind value,uint64_t parentID=0) noexcept:kind(value) {
        if(!Active()) return;
        started=clockNS();capturedEpoch=epoch;capturedStep=stepID;capturedCall=callID;parent=parentID;
        scope=++nextScope;startWindow=windowID;startedCall=nextCall;previous=activeScope;
        parentScope=previous?previous->scope:0;activeScope=this;
        exceptions=std::uncaught_exceptions();window.overheadNS+=clockNS()-started;
    }
    EventScope(const EventScope&)=delete;
    EventScope& operator=(const EventScope&)=delete;
    void SetWorkBytes(uint64_t value) noexcept {workBytes=value;}
    void Finish(bool failure=false) noexcept {
        if(finished) return;
        finished=true;
        if(!capturedEpoch || !Active() || epoch!=capturedEpoch) return;
        const auto finishedAt=clockNS(),elapsed=finishedAt>=started ? finishedAt-started : 0;
        if(previous)previous->childNS+=elapsed;
        if(activeScope==this)activeScope=previous;
        auto& metric=window.metrics[size_t(kind)];++metric.calls;metric.wallNS+=elapsed;
        metric.maxNS=std::max(metric.maxNS,elapsed);metric.failures+=failure || std::uncaught_exceptions()>exceptions;
        metric.workBytes+=workBytes;
        if(elapsed>=16000000) {
            ++window.eligible;
            Detail detail;detail.kind=kind;detail.step=capturedStep;detail.call=capturedCall;detail.parent=parent;
            detail.scope=scope;detail.parentScope=parentScope;detail.startWindow=startWindow;
            detail.wallNS=elapsed;detail.childWallNS=std::min(childNS,elapsed);detail.bytes=workBytes;
            // Reuse the owner-thread call serial; no per-opcode timer or lookup.
            detail.vmCalls=(nextCall>=startedCall ? nextCall-startedCall : 0)+uint64_t(kind==Kind::VM);
            detail.failed=failure || std::uncaught_exceptions()>exceptions;
            detail.role=kind==Kind::Step ? 0 : kind==Kind::VM && !parent ? 1 : kind>=Kind::PSBOpen ? 2 : 3;
            Select(detail);
        }
        if(epoch==capturedEpoch)window.overheadNS+=clockNS()-finishedAt;
    }
    ~EventScope() noexcept {Finish();}
};
class CallScope {
    uint64_t previous=0,capturedEpoch=0;
    EventScope event;
    static uint64_t Enter(uint64_t& previous,uint64_t& savedEpoch) noexcept {
        if(!Active()) return 0;
        const auto begin=clockNS();previous=callID;savedEpoch=epoch;callID=++nextCall;
        window.overheadNS+=clockNS()-begin;return previous;
    }
public:
    CallScope() noexcept:event(Kind::VM,Enter(previous,capturedEpoch)) {}
    void Finish(bool failed=false) noexcept {event.Finish(failed);}
    ~CallScope() noexcept {event.Finish();if(capturedEpoch && epoch==capturedEpoch) callID=previous;}
};
class StepScope {
    uint64_t previousStep=0,previousCall=0,capturedEpoch=0;
    std::optional<FrameCandidates> frame;
    EventScope event;
    Kind Enter() noexcept {
        if(Active()) {
            const auto begin=clockNS();capturedEpoch=epoch;previousStep=stepID;previousCall=callID;
            stepID=++nextStep;callID=0;window.overheadNS+=clockNS()-begin;
            frame.emplace();frame->group.step=stepID;frame->previous=activeFrame;activeFrame=&*frame;
        }
        return Kind::Step;
    }
public:
    StepScope() noexcept:event(Enter()) {}
    ~StepScope() noexcept {event.Finish();if(capturedEpoch && epoch==capturedEpoch) {
        Merge(frame->group);activeFrame=frame->previous;
        stepID=previousStep;callID=previousCall;}}
};
inline void EmitDetail(const Detail& d,uint64_t takenID) noexcept {
    const auto now=clockNS();
    while(emissionCount && now>=emissions[0].time && now-emissions[0].time>=1000000000) {
        byteUsed-=emissions[0].bytes;
        for(size_t i=1;i<emissionCount;++i)emissions[i-1]=emissions[i];
        --emissionCount;
    }
    if(byteUsed+961>8192 || emissionCount==emissions.size()) {Drop(window.byteDropped);return;}
    char line[961]{};
    const int prefix=std::snprintf(line,sizeof(line),
        "cpu.slow v=2 epoch=%llu window=%llu stepID=%llu callID=%llu parentCallID=%llu kind=%s wallNS=%llu failed=%u scopeID=%llu parentScopeID=%llu startWindow=%llu childWallNS=%llu residualWallNS=%llu bytes=%llu vmCalls=%llu role=%u stackTruncated=0 stack=\"",
        (unsigned long long)epoch,(unsigned long long)takenID,(unsigned long long)d.step,
        (unsigned long long)d.call,(unsigned long long)d.parent,names[size_t(d.kind)],
        (unsigned long long)d.wallNS,unsigned(d.failed),(unsigned long long)d.scope,
        (unsigned long long)d.parentScope,(unsigned long long)d.startWindow,
        (unsigned long long)d.childWallNS,(unsigned long long)(d.wallNS-d.childWallNS),
        (unsigned long long)d.bytes,(unsigned long long)d.vmCalls,d.role);
    if(prefix<=0 || size_t(prefix)+3>=sizeof(line)) {Drop(window.formatDropped);return;}
    const bool complete=Escape(line+prefix,std::min<size_t>(513,sizeof(line)-size_t(prefix)-1),d.stack);
    if(!complete)std::strstr(line,"stackTruncated=0")[15]='1';
    const size_t used=std::strlen(line);line[used]='"';line[used+1]=0;
    const uint64_t savedEpoch=epoch;
    const bool emitted=Emit(line);
    if(epoch!=savedEpoch || !Active()) return;
    if(emitted) {
        ++window.emitted;byteUsed+=used+2;emissions[emissionCount++]={now,uint64_t(used+2)};
    } else Drop(window.callbackDropped);
}
inline void TakeWindow(uint64_t takenID,uint64_t nextID) noexcept {
    if(!Active()) return;
    const auto begin=clockNS(),savedEpoch=epoch;
    Merge(outsideCandidates);outsideCandidates={};
    // A window can close inside a reentrant host step. Flush completed child
    // candidates to that window so eligible/emitted/drop accounting stays exact.
    for(auto* f=activeFrame;f;f=f->previous) {
        Merge(f->group);const auto step=f->group.step;f->group={};f->group.step=step;
    }
    const auto selected=candidates;candidates={};
    // Selection is frozen. Stack capture already happened before VM unwind.
    for(const auto& group:selected)for(const auto& detail:group.details)if(detail.used) {
        EmitDetail(detail,takenID);
        if(epoch!=savedEpoch || !Active()) return;
    }
    const auto captured=window;window=Window{};windowID=nextID;
    char line[961]{};
    std::snprintf(line,sizeof(line),"cpu.summary v=2 epoch=%llu window=%llu eligible=%llu emitted=%llu dropped=%llu frameDropped=%llu windowDropped=%llu byteDropped=%llu formatDropped=%llu callbackDropped=%llu stackFailures=%llu callbackFailures=%llu overheadNS=%llu selection=2",
        (unsigned long long)epoch,(unsigned long long)takenID,(unsigned long long)captured.eligible,
        (unsigned long long)captured.emitted,(unsigned long long)captured.dropped,(unsigned long long)captured.frameDropped,
        (unsigned long long)captured.windowDropped,(unsigned long long)captured.byteDropped,
        (unsigned long long)captured.formatDropped,(unsigned long long)captured.callbackDropped,
        (unsigned long long)captured.stackFailures,(unsigned long long)captured.callbackFailures,(unsigned long long)captured.overheadNS);
    Emit(line);if(epoch!=savedEpoch || !Active()) return;
    for(size_t i=0;i<captured.metrics.size();++i) {
        const auto& m=captured.metrics[i];
        std::snprintf(line,sizeof(line),"cpu.aggregate v=2 epoch=%llu window=%llu kind=%s calls=%llu wallNS=%llu maxNS=%llu failures=%llu bytes=%llu",
            (unsigned long long)epoch,(unsigned long long)takenID,names[i],(unsigned long long)m.calls,
            (unsigned long long)m.wallNS,(unsigned long long)m.maxNS,(unsigned long long)m.failures,(unsigned long long)m.workBytes);
        Emit(line);if(epoch!=savedEpoch || !Active()) return;
    }
    window.overheadNS+=clockNS()-begin;
}
}
