#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// Native plugin diagnostics only. No texture/script ownership or GPU queries.
namespace krkrsdl3::layer_image {
inline constexpr size_t Capacity=64;
struct Metrics {
    uint64_t calls=0,readCalls=0,readBytes=0,readWallNS=0,readWaitNS=0,parameterBytes=0;
};
struct Context {
    const char* method="unknown";
    const char* stage="invoke";
    const char* route="cpu";
    const char* reason="unsupportedMethod";
    char parameters[128]="unconverted";
    uint64_t epoch=0,windowID=0;
    Metrics metrics;
    bool entered=false,allowGPU=false;
    void* receiver=nullptr;
};
inline thread_local Context* context=nullptr;
struct Row {
    char method[32]{},stage[16]{},route[16]{},reason[32]{},parameters[128]{};
    Metrics metrics;
};
inline bool Add(uint64_t& target,uint64_t value) noexcept {
    if(value>std::numeric_limits<uint64_t>::max()-target) {
        target=std::numeric_limits<uint64_t>::max();return false;
    }
    target+=value;return true;
}
inline bool Add(Metrics& target,const Metrics& value) noexcept {
    bool ok=true;
    for(auto member:{&Metrics::calls,&Metrics::readCalls,&Metrics::readBytes,
                     &Metrics::readWallNS,&Metrics::readWaitNS,&Metrics::parameterBytes})
        if(!Add(target.*member,value.*member)) ok=false;
    return ok;
}
template<size_t N> inline bool Copy(char (&out)[N],const char* value) noexcept {
    if(!value || std::strlen(value)>=N) return false;
    std::memcpy(out,value,std::strlen(value)+1);return true;
}
struct Window {
    std::array<Row,Capacity> rows{};size_t size=0;
    Metrics totals,overflow;
    uint64_t capacityRecords=0,oversizeRecords=0,lateCalls=0,lateReads=0;
    uint64_t calls=0,constructors=0,gpuCalls=0,cpuCalls=0,noopCalls=0,errorCalls=0;
    bool saturated=false;
    void Record(const Context& c) noexcept {
        if(!Add(totals,c.metrics)) saturated=true;
        auto count=[&](uint64_t& value){if(!Add(value,c.metrics.calls)) saturated=true;};
        if(!std::strcmp(c.stage,"construct")) count(constructors);
        else {
            count(calls);
            if(!std::strcmp(c.route,"gpu")) count(gpuCalls);
            else if(!std::strcmp(c.route,"cpu")) count(cpuCalls);
            else if(!std::strcmp(c.route,"noop")) count(noopCalls);
            else count(errorCalls);
        }
        for(size_t i=0;i<size;++i) {
            auto& r=rows[i];
            if(!std::strcmp(r.method,c.method) && !std::strcmp(r.stage,c.stage) &&
               !std::strcmp(r.route,c.route) && !std::strcmp(r.reason,c.reason) &&
               !std::strcmp(r.parameters,c.parameters)) {
                if(!Add(r.metrics,c.metrics)) saturated=true;
                return;
            }
        }
        Row row;
        if(!Copy(row.method,c.method) || !Copy(row.stage,c.stage) || !Copy(row.route,c.route) ||
           !Copy(row.reason,c.reason) || !Copy(row.parameters,c.parameters)) {
            if(!Add(oversizeRecords,1)) saturated=true;
            if(!Add(overflow,c.metrics)) saturated=true;
            return;
        }
        if(size==Capacity) {
            if(!Add(capacityRecords,1)) saturated=true;
            if(!Add(overflow,c.metrics)) saturated=true;
            return;
        }
        row.metrics=c.metrics;rows[size++]=row;
    }
};
}
