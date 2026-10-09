#pragma once
#include "LayerSpanRouteDiagnostics.h"

// Successful whole-texture reads only. No resource ownership or timing query.
namespace krkrsdl3::cpu_reads {
inline constexpr size_t Capacity=64;
inline constexpr std::array<const char*,8> ProtectedMethods{{
    "drawImageStretch","drawPath","drawLine","drawRectangle","clear",
    "alphaToProvince","Layer.saveLayerImage","getRecordImage"}};
struct Metrics {uint64_t calls=0,bytes=0,wallNS=0,waitNS=0;};
struct Input {
    const char* method;const char* entry;const char* access;const char* origin;
    bool valid;
    uint64_t windowID=0;
    bool detail=false,caller=false;
};
inline bool Accumulate(Metrics& to,const Metrics& from) {
    bool ok=span_route::Add(to.calls,from.calls);
    if(!span_route::Add(to.bytes,from.bytes)) ok=false;
    if(!span_route::Add(to.wallNS,from.wallNS)) ok=false;
    if(!span_route::Add(to.waitNS,from.waitNS)) ok=false;
    return ok;
}
struct Group {
    bool used=false;
    char method[49]{},entry[49]{},access[49]{},origin[49]{};
    Metrics metrics;
    bool Matches(const char* m,const char* e,const char* a,const char* o) const {
        return used && !std::strcmp(method,m) && !std::strcmp(entry,e) &&
            !std::strcmp(access,a) && !std::strcmp(origin,o);
    }
};
struct Window {
    std::array<Group,Capacity> groups{};
    Metrics totals,capacityOverflow,oversizeOverflow;
    uint64_t capacityRecords=0,oversizeRecords=0,repeatedReads=0;
    bool overflow=false;
    void Record(const char* m,const char* e,const char* a,const char* o,const Metrics& value,bool labelsValid) {
        if(!Accumulate(totals,value)) overflow=true;
        if(!labelsValid) {
            if(!Accumulate(oversizeOverflow,value) || !span_route::Add(oversizeRecords,1)) overflow=true;
            return;
        }
        size_t slot=Capacity;
        for(size_t i=0;i<Capacity;++i) if(groups[i].Matches(m,e,a,o)) {slot=i;break;}
        if(slot<Capacity) {if(!span_route::Add(repeatedReads,1)) overflow=true;}
        else {
            for(size_t i=0;i<ProtectedMethods.size();++i)
                if(!std::strcmp(ProtectedMethods[i],m) && !groups[i].used) {slot=i;break;}
            if(slot==Capacity) for(size_t i=ProtectedMethods.size();i<Capacity;++i)
                if(!groups[i].used) {slot=i;break;}
        }
        if(slot==Capacity) {
            if(!Accumulate(capacityOverflow,value) || !span_route::Add(capacityRecords,1)) overflow=true;
            return;
        }
        auto& g=groups[slot];
        if(!g.used) {
            g.used=true;std::strcpy(g.method,m);std::strcpy(g.entry,e);
            std::strcpy(g.access,a);std::strcpy(g.origin,o);
        }
        if(!Accumulate(g.metrics,value)) overflow=true;
    }
};
}
