#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

// Native-only route decisions. Fixed dictionaries bound memory and output;
// samples hold scalar identities, never textures, leases or script objects.
namespace krkrsdl3::span_route {
inline constexpr std::array<const char*,5> Methods{{"drawLine","drawPath","drawImageStretch","drawRectangle","clear"}};
inline constexpr std::array<const char*,3> Routes{{"gpu","cpu","noop"}};
inline constexpr std::array<const char*,25> Reasons{{
    "none","applied","arguments","appearanceType","pathType","imageType","numeric","paint","path",
    "source","borrowedSource","vectorSource","record","activeLease","targetAlias","state",
    "backendUnavailable","target","resource","unsupported","geometry","budget","backend","sampling","nonNativeReceiver"}};
inline constexpr size_t ProtectedSamples=Methods.size()*Routes.size(), MaxSamples=32;
inline constexpr size_t GroupCount=ProtectedSamples*Reasons.size();
template<size_t N> inline size_t Find(const std::array<const char*,N>& names,const char* value) {
    if(value) for(size_t i=0;i<N;++i) if(!std::strcmp(names[i],value)) return i;
    return N;
}
inline bool Add(uint64_t& target,uint64_t value) {
    if(value>std::numeric_limits<uint64_t>::max()-target) {target=std::numeric_limits<uint64_t>::max();return false;}
    target+=value;return true;
}
struct Metrics {uint64_t calls=0,spanCount=0,sourceBytes=0,parameterBytes=0,scratchBytes=0;};
inline bool Accumulate(Metrics& to,const Metrics& from) {
    bool ok=Add(to.calls,from.calls);
    if(!Add(to.spanCount,from.spanCount)) ok=false;
    if(!Add(to.sourceBytes,from.sourceBytes)) ok=false;
    if(!Add(to.parameterBytes,from.parameterBytes)) ok=false;
    if(!Add(to.scratchBytes,from.scratchBytes)) ok=false;
    return ok;
}
struct Group {Metrics metrics;int sample=-1;};
struct Sample {
    bool used=false,identity=false;
    size_t group=0;
    uint64_t traceID=0,sessionID=0,textureID=0,contentVersion=0;
    Metrics metrics;
};
struct Window {
    uint64_t id=0;
    Metrics totals;
    std::array<uint64_t,3> routes{};
    std::array<Group,GroupCount> groups{};
    std::array<Sample,MaxSamples> samples{};
    size_t extraSamples=0;
    uint64_t repeatedOmitted=0,capacityOmitted=0,invalidRecords=0;
    bool overflow=false;
    // Return an admitted sample slot; -1 means no resource identity query.
    int Record(const char* method,const char* route,const char* reason,const Metrics& metrics,uint64_t traceID) {
        if(!Accumulate(totals,metrics)) overflow=true;
        const size_t m=Find(Methods,method),r=Find(Routes,route),why=Find(Reasons,reason);
        if(m==Methods.size() || r==Routes.size() || why==Reasons.size()) {
            if(!Add(invalidRecords,1)) overflow=true;
            return -1;
        }
        if(!Add(routes[r],metrics.calls)) overflow=true;
        const size_t pair=m*Routes.size()+r,index=pair*Reasons.size()+why;
        auto& group=groups[index];
        if(!Accumulate(group.metrics,metrics)) overflow=true;
        if(group.sample>=0) {if(!Add(repeatedOmitted,1)) overflow=true;return -1;}
        // A group's failed first admission stays an explicit capacity omission.
        if(group.metrics.calls!=1) {if(!Add(capacityOmitted,1)) overflow=true;return -1;}
        size_t slot=pair;
        if(samples[slot].used) {
            if(extraSamples>=MaxSamples-ProtectedSamples) {if(!Add(capacityOmitted,1)) overflow=true;return -1;}
            slot=ProtectedSamples+extraSamples++;
        }
        group.sample=int(slot);
        auto& sample=samples[slot];sample.used=true;sample.group=index;sample.traceID=traceID;sample.metrics=metrics;
        return int(slot);
    }
};
}
