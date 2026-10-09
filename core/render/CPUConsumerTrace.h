#pragma once
#include "LayerWorkDiagnostics.h"
#include <cstdint>

// Borrowed fixed entry labels and opaque IDs only: no script/resource ownership.
namespace krkrsdl3::cpu_consumer_trace {
enum class Access { Metadata, Read, Write };
inline const char* Name(Access value) {
    return value==Access::Read ? "read" : value==Access::Write ? "write" : "metadata";
}
struct Context {
    const char* method="unknown";
    const char* nativeEntry="unknown";
    Access access=Access::Read;
    uintptr_t owner=0;
    uint64_t traceID=0,epoch=0;
    mutable bool shrinkSucceeded=false;
};
inline thread_local const Context* context=nullptr;
inline std::atomic<uint64_t> nextTrace{1},nextRead{1},nextSession{1};
inline void (*logMessage)(const char*)=nullptr;
inline bool Enabled() { return layer_work::enabled.load(std::memory_order_relaxed) && logMessage; }
inline const Context* Current() { return Enabled() ? context : nullptr; }
inline const char* CurrentMethod() { const auto* c=Current();return c ? c->method : "unknown"; }
inline bool IsShrinkMethod(const Context* c) {
    return c && (!std::strcmp(c->method,"Layer.shrinkCopy") || !std::strcmp(c->method,"Layer.shrinkCopyFast"));
}
inline void MarkShrinkSuccess() {
    if(Enabled() && IsShrinkMethod(context)) context->shrinkSucceeded=true;
}
class ConsumerScope {
    Context value;
    const Context* previous;
public:
    ConsumerScope(const char* method,Access access,const char* nativeEntry,uintptr_t owner=0,bool preserveOuter=false)
        :previous(context) {
        if(Enabled()) {
            value.epoch=layer_work::CaptureGeneration();
            if(preserveOuter && previous) value=*previous;
            else {
                value.method=method ? method : "unknown";value.access=access;
                value.nativeEntry=nativeEntry ? nativeEntry : "unknown";value.owner=owner;
                value.traceID=nextTrace.fetch_add(1,std::memory_order_relaxed);
            }
        }
        // An inactive call masks older context if diagnostics are toggled inside it.
        context=&value;
    }
    ~ConsumerScope() {context=previous;}
    ConsumerScope(const ConsumerScope&)=delete;
    ConsumerScope& operator=(const ConsumerScope&)=delete;
};
struct Read {
    Context consumer;
    uint64_t readID=0,epoch=0,sessionID=0,textureID=0,contentVersion=0;
    const char* source="unattributed";
    const char* lastWriter="unknown";
    int width=0,height=0,lastWrite[4]{};
    uint64_t bytes=0,wallNS=0,waitNS=0;
};
inline Read BeginRead(uint64_t epoch,uint64_t sessionID) {
    Read read;
    if(!epoch || !Enabled()) return read;
    if(context && context->epoch!=epoch) return read;
    read.epoch=epoch;read.sessionID=sessionID;
    if(context) read.consumer=*context;
    else {read.consumer.epoch=epoch;read.consumer.traceID=nextTrace.fetch_add(1,std::memory_order_relaxed);}
    read.readID=nextRead.fetch_add(1,std::memory_order_relaxed);
    return read;
}
// Both the raw and escaped UTF-8 bounds matter: control characters can expand
// sixfold in JSON. Never cut a multibyte code point at a byte boundary.
inline std::string BoundedUTF8(const char* input,size_t rawLimit,size_t escapedLimit) {
    if(!input) return {};
    size_t raw=0,escaped=0;
    while(input[raw]) {
        const auto lead=static_cast<unsigned char>(input[raw]);
        size_t count=lead<0x80 ? 1 : lead>=0xc2 && lead<=0xdf ? 2 :
            lead>=0xe0 && lead<=0xef ? 3 : lead>=0xf0 && lead<=0xf4 ? 4 : 0;
        if(!count || raw+count>rawLimit) break;
        bool valid=true;
        for(size_t i=1;i<count;++i) if(!input[raw+i] || (static_cast<unsigned char>(input[raw+i])&0xc0)!=0x80) {valid=false;break;}
        if(count>=3) {
            const auto second=static_cast<unsigned char>(input[raw+1]);
            if((lead==0xe0 && second<0xa0) || (lead==0xed && second>=0xa0) ||
               (lead==0xf0 && second<0x90) || (lead==0xf4 && second>=0x90)) valid=false;
        }
        if(!valid) break;
        const size_t cost=count==1 ? lead<32 ? 6 : lead=='"' || lead=='\\' ? 2 : 1 : count;
        if(escaped+cost>escapedLimit) break;
        raw+=count;escaped+=cost;
    }
    return std::string(input,raw);
}
inline std::string Label(const char* label) {
    return layer_work::JSONString(label ? label : "");
}
inline bool ValidLabel(const char* label) {
    if(!label) return true;
    return BoundedUTF8(label,48,48).size()==std::strlen(label);
}
inline bool CurrentEpoch(uint64_t epoch) {
    return epoch && Enabled() && epoch==layer_work::generation.load(std::memory_order_relaxed);
}
inline bool Reserve(uint64_t epoch,bool producer,bool& caller) {
    caller=false;
    if(!CurrentEpoch(epoch)) return false;
    std::lock_guard<std::mutex> lock(layer_work::mutex);
    if(!CurrentEpoch(epoch)) return false;
    auto& b=layer_work::profile.cpuConsumerBudget;
    auto& used=producer ? b.producers : b.reads;
    auto& exceeded=producer ? b.producerExceeded : b.readExceeded;
    if(used>=32) {++exceeded;return false;}
    ++used;
    return true;
}
inline bool ReserveCaller(uint64_t epoch) {
    if(!CurrentEpoch(epoch)) return false;
    std::lock_guard<std::mutex> lock(layer_work::mutex);
    if(!CurrentEpoch(epoch)) return false;
    auto& b=layer_work::profile.cpuConsumerBudget;
    if(b.callers>=8) {++b.callerExceeded;return false;}
    ++b.callers;return true;
}
inline void RejectOversize(uint64_t epoch) {
    std::lock_guard<std::mutex> lock(layer_work::mutex);
    if(CurrentEpoch(epoch)) ++layer_work::profile.cpuConsumerBudget.oversize;
}
inline bool Emit(uint64_t epoch,const std::string& message) {
    if(!CurrentEpoch(epoch)) return false;
    if(message.size()>900) {
        RejectOversize(epoch);
        return false;
    }
    logMessage(message.c_str());return true;
}
inline std::string IDs(const Read& read) {
    return "\"readID\":"+std::to_string(read.readID)+",\"traceID\":"+std::to_string(read.consumer.traceID)+
        ",\"generation\":"+std::to_string(read.epoch)+",\"sessionID\":"+std::to_string(read.sessionID);
}
inline bool ReportRead(const Read& r) noexcept {
    try {
        bool caller=false;
        if(!Reserve(r.epoch,false,caller)) return false;
        // An oversized native name is rejected, never shortened to a prefix
        // that could falsely match a different C0 transfer origin or method.
        if(!ValidLabel(r.consumer.method) || !ValidLabel(r.consumer.nativeEntry) ||
           !ValidLabel(r.source) || !ValidLabel(r.lastWriter)) {RejectOversize(r.epoch);return false;}
        std::string message="metal.cpuConsumer {\"phase\":\"read\","+IDs(r)+
            ",\"textureID\":"+std::to_string(r.textureID)+",\"contentVersion\":"+std::to_string(r.contentVersion)+
            ",\"method\":"+Label(r.consumer.method)+",\"access\":"+Label(Name(r.consumer.access))+
            ",\"nativeEntry\":"+Label(r.consumer.nativeEntry)+",\"owner\":"+std::to_string(r.consumer.owner)+
            ",\"source\":"+Label(r.source)+",\"width\":"+std::to_string(r.width)+",\"height\":"+std::to_string(r.height)+
            ",\"lastWriter\":"+Label(r.lastWriter)+",\"lastWrite\":[";
        for(size_t i=0;i<4;++i) {if(i) message+=',';message+=std::to_string(r.lastWrite[i]);}
        message+="],\"lastSubmittedID\":null,\"renderFrame\":null,\"calls\":1,\"bytes\":"+std::to_string(r.bytes)+
            ",\"wallNS\":"+std::to_string(r.wallNS)+",\"waitNS\":"+std::to_string(r.waitNS)+'}';
        return Emit(r.epoch,message) && ReserveCaller(r.epoch);
    } catch(...) {return false;}
}
inline void ReportCaller(const Read& r,const char* stack) noexcept {
    try {
        if(!CurrentEpoch(r.epoch)) return;
        const auto bounded=BoundedUTF8(stack,512,512);
        const bool available=!bounded.empty();
        Emit(r.epoch,"metal.cpuConsumer {\"phase\":\"caller\","+IDs(r)+
            ",\"traceState\":\""+(available ? "captured" : "unavailable")+"\",\"positions\":\""+
            (available ? "unverified" : "unavailable")+"\",\"trace\":"+layer_work::JSONString(bounded.c_str())+'}');
    } catch(...) {}
}
struct Producer {
    uint64_t sessionID=0,textureID=0,contentVersion=0;
    int width=0,height=0;
};
inline bool (*captureProducer)(void*,Producer&)=nullptr;
// Native-only, bounded route evidence for the C2B transaction. Packet and GPU
// scratch bytes are separate from C0 pixel transfers; reporting never acquires
// pixels, submits work or waits for the GPU.
inline void ReportSpanRoute(const char* method,const char* route,const char* reason,
        void* target,uint64_t spanCount,uint64_t sourceBytes,uint64_t parameterBytes,
        uint64_t scratchBytes) noexcept {
    if(!Enabled() || !context || !CurrentEpoch(context->epoch)) return;
    try {
        const auto c=*context;
        int slot=-1;uint64_t windowID=0;
        {
            std::lock_guard<std::mutex> lock(layer_work::mutex);
            if(!CurrentEpoch(c.epoch)) return;
            auto& b=layer_work::profile.cpuConsumerBudget;
            slot=b.spanWindow.Record(method,route,reason,{1,spanCount,sourceBytes,parameterBytes,scratchBytes},c.traceID);
            windowID=b.spanWindow.id;
        }
        if(slot<0) return;
        Producer p;
        bool identity=false;
        try {identity=target && captureProducer && captureProducer(target,p);} catch(...) {}
        // A concurrent Take can already have emitted the admitted sample with
        // unknown identity. Never attach old metadata to the following window.
        std::lock_guard<std::mutex> lock(layer_work::mutex);
        if(!CurrentEpoch(c.epoch) || layer_work::profile.cpuConsumerBudget.spanWindow.id!=windowID) return;
        auto& sample=layer_work::profile.cpuConsumerBudget.spanWindow.samples[size_t(slot)];
        sample.identity=identity;sample.sessionID=p.sessionID;sample.textureID=p.textureID;sample.contentVersion=p.contentVersion;
    } catch(...) {}
}
inline void ReportShrinkOutput(void* texture,int left,int top,int right,int bottom) noexcept {
    if(!Enabled() || !captureProducer || !context || !CurrentEpoch(context->epoch)) return;
    try {
        const auto c=*context;Producer p;
        if(!captureProducer(texture,p)) return;
        bool caller=false;
        if(!Reserve(c.epoch,true,caller)) return;
        if(!ValidLabel(c.method) || !ValidLabel(c.nativeEntry)) {RejectOversize(c.epoch);return;}
        Emit(c.epoch,"metal.cpuProducer {\"phase\":\"shrink\",\"traceID\":"+std::to_string(c.traceID)+
            ",\"generation\":"+std::to_string(c.epoch)+",\"sessionID\":"+std::to_string(p.sessionID)+
            ",\"textureID\":"+std::to_string(p.textureID)+",\"contentVersion\":"+std::to_string(p.contentVersion)+
            ",\"method\":"+Label(c.method)+",\"nativeEntry\":"+Label(c.nativeEntry)+
            ",\"outputROI\":["+std::to_string(left)+','+std::to_string(top)+','+std::to_string(right)+','+
            std::to_string(bottom)+"],\"width\":"+std::to_string(p.width)+",\"height\":"+std::to_string(p.height)+'}');
    } catch(...) {}
}
inline std::string SpanMetricsJSON(const span_route::Metrics& m) {
    return "\"calls\":"+std::to_string(m.calls)+",\"spanCount\":"+std::to_string(m.spanCount)+
        ",\"sourceBytes\":"+std::to_string(m.sourceBytes)+",\"parameterBytes\":"+std::to_string(m.parameterBytes)+
        ",\"scratchBytes\":"+std::to_string(m.scratchBytes);
}
inline std::string SpanLabelsJSON(size_t group) {
    const size_t pair=group/span_route::Reasons.size();
    return "\"method\":"+Label(span_route::Methods[pair/span_route::Routes.size()])+
        ",\"route\":"+Label(span_route::Routes[pair%span_route::Routes.size()])+
        ",\"reason\":"+Label(span_route::Reasons[group%span_route::Reasons.size()]);
}
inline bool EmitSpan(uint64_t epoch,const std::string& message) noexcept {
    try {return Emit(epoch,message);} catch(...) {return false;}
}
inline void SpanWindowTaken(const span_route::Window& w,uint64_t epoch) noexcept {
    if(!CurrentEpoch(epoch)) return;
    try {
        const auto base="\"version\":2,\"generation\":"+std::to_string(epoch)+",\"windowID\":"+std::to_string(w.id)+',';
        size_t groups=0,samples=0;
        for(size_t i=0;i<w.groups.size();++i) if(w.groups[i].metrics.calls) {
            ++groups;
            EmitSpan(epoch,"metal.layerSpan {\"phase\":\"aggregate\","+base+SpanLabelsJSON(i)+','+
                SpanMetricsJSON(w.groups[i].metrics)+'}');
        }
        for(size_t i=0;i<w.samples.size();++i) if(w.samples[i].used) {
            ++samples;const auto& s=w.samples[i];
            const auto identity=[&](uint64_t value){return s.identity ? std::to_string(value) : std::string("null");};
            EmitSpan(epoch,"metal.layerSpan {\"phase\":\"sample\","+base+SpanLabelsJSON(s.group)+
                ",\"sampleIndex\":"+std::to_string(i)+",\"traceID\":"+std::to_string(s.traceID)+
                ",\"sessionID\":"+identity(s.sessionID)+",\"textureID\":"+identity(s.textureID)+
                ",\"contentVersion\":"+identity(s.contentVersion)+','+SpanMetricsJSON(s.metrics)+'}');
        }
        EmitSpan(epoch,"metal.layerSpan {\"phase\":\"window\","+base+SpanMetricsJSON(w.totals)+
            ",\"gpuCalls\":"+std::to_string(w.routes[0])+",\"cpuCalls\":"+std::to_string(w.routes[1])+
            ",\"noopCalls\":"+std::to_string(w.routes[2])+",\"aggregateRows\":"+std::to_string(groups)+
            ",\"samples\":"+std::to_string(samples)+",\"repeatedOmitted\":"+std::to_string(w.repeatedOmitted)+
            ",\"capacityOmitted\":"+std::to_string(w.capacityOmitted)+",\"invalidRecords\":"+std::to_string(w.invalidRecords)+
            ",\"overflow\":"+(w.overflow ? "true" : "false")+'}');
    } catch(...) {}
}
inline void WindowTaken(const layer_work::CPUConsumerBudget& b,uint64_t epoch) {
    SpanWindowTaken(b.spanWindow,epoch);
    if(!CurrentEpoch(epoch) || !(b.reads || b.producers || b.readExceeded || b.producerExceeded ||
            b.spanRoutes || b.spanRouteExceeded)) return;
    try {
        Emit(epoch,"metal.cpuConsumer {\"phase\":\"budget\",\"generation\":"+std::to_string(epoch)+
            ",\"readRecords\":"+std::to_string(b.reads)+",\"readExceeded\":"+std::to_string(b.readExceeded)+
            ",\"callerRecords\":"+std::to_string(b.callers)+",\"callerExceeded\":"+std::to_string(b.callerExceeded)+
            ",\"producerRecords\":"+std::to_string(b.producers)+",\"producerExceeded\":"+std::to_string(b.producerExceeded)+
            ",\"oversizeRecords\":"+std::to_string(b.oversize)+
            ",\"spanRouteRecords\":"+std::to_string(b.spanRoutes)+
            ",\"spanRouteExceeded\":"+std::to_string(b.spanRouteExceeded)+'}');
    } catch(...) {}
}
inline void SetCallbacks(void (*logger)(const char*),bool (*producer)(void*,Producer&)) {
    logMessage=logger;captureProducer=producer;layer_work::cpuConsumerWindowTaken=WindowTaken;
}
}
