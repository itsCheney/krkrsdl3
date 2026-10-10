#pragma once
#include "../LayerHotspotContext.h"
#include "../LayerRenderOperation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// CPU metadata only. Completion handlers retain a frozen sample, never a Layer,
// Resource, Metal texture, or Impl. Logical pixel counts are not bus traffic or
// a claim about the number of fragment invocations after load-action clears.
namespace krkrsdl3 { namespace layer_hotspot {
inline constexpr size_t OperationCapacity=256, ResourceCapacity=64;
inline constexpr size_t MessageBytes=960, OutputBytes=32*1024, ChunkBytes=720;
enum class Access : unsigned { Rect, Read, Snapshot, Upload, Copy, Affine,
    Perspective, Span, Shrink, Transition, Window, Mesh, Create, Clear, Pass, End, Execution };
enum class Execution : unsigned { Immediate, Deferred, Materialized, Omitted };
enum class Boundary : unsigned { Target, Upload, Blit, Compute, Mesh, Submit,
    Destroy, Read, Snapshot, Window, ExplicitClear, Initialization, Other, Count };
enum class ClearOrigin : unsigned { LayerCreate, OtherCreate, FirstRead, FirstWrite,
    FusedInitialization, FullFill, Explicit, Count };
inline constexpr const char* BoundaryNames[]={"target","upload","blit","compute","mesh","submit",
    "destroy","read","snapshot","window","explicitClear","initialization","other"};
inline constexpr const char* ClearNames[]={"layerCreate","otherCreate","firstRead","firstWrite",
    "fusedInitialization","fullFill","explicit"};
struct ResourceInfo {
    uint64_t generation=0;
    Identity identity;
    int width=0,height=0,bytesPerPixel=4;
};
struct Totals {
    uint64_t calls=0,pixels=0,scaledPixels=0,aliasPixels=0,snapshotBytes=0;
    uint64_t render=0,compute=0,blit=0,draws=0,creates=0,clearBytes=0;
    uint64_t elidedInitialization=0,fusedInitialization=0,fastFills=0,sampleFailures=0,diagnosticGaps=0;
    uint64_t deferredFills=0,materializedFills=0,omittedFills=0,actualDraws=0;
    bool saturated=false;
    std::array<uint64_t,TVP_LAYER_OPERATION_COUNT> kindPixels{};
    std::array<uint64_t,size_t(Boundary::Count)> ends{};
    std::array<uint64_t,size_t(ClearOrigin::Count)> clears{};
    void Add(uint64_t& value,uint64_t amount=1) {
        if(amount>std::numeric_limits<uint64_t>::max()-value) {
            value=std::numeric_limits<uint64_t>::max();saturated=true;
        } else value+=amount;
    }
};
struct Operation {
    uint64_t receiver=0,logicalID=0;
    unsigned receiverKind=0,flags=0,color=0,sampling=0;
    int opacity=255;
    Execution execution=Execution::Immediate;
    uint64_t sequence=0,layer=0,encoder=0,targetVersion=0,sourceVersion=0,pixels=0,snapshotBytes=0;
    int target=-1,source=-1,kind=0,opaque=-1,reason=0;
    Access access=Access::Rect;
    bool full=false,readsTarget=false,scaled=false,alias=false;
    std::array<int,4> destination{},sourceRect{},clip{};
    char role[24]{};
};
struct Sample {
    uint64_t epoch=0,command=0,firstFrame=0,lastFrame=0,sequence=0;
    uint64_t operationOverflow=0,resourceOverflow=0,windowPixels=0;
    double drawableMS=0,frameMS=0;
    bool drawableObserved=false;
    size_t resourceCount=0,operationCount=0;
    std::array<ResourceInfo,ResourceCapacity> resources{};
    std::array<Operation,OperationCapacity> operations{};
    Totals totals;
};
inline uint64_t NextEpoch() {static std::atomic<uint64_t> id{1};return id.fetch_add(1);}
inline std::string JSON(const char* input) {
    std::string out="\"";constexpr char hex[]="0123456789abcdef";
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(input);*p;++p) {
        if(*p=='"' || *p=='\\') {out+='\\';out+=char(*p);}
        else if(*p<32) {out+="\\u00";out+=hex[*p>>4];out+=hex[*p&15];}
        else out+=char(*p);
    }
    return out+'"';
}
template<class Range> inline std::string Numbers(const Range& values) {
    std::string out="[";for(const auto& n:values){if(out.size()>1)out+=',';out+=std::to_string(n);}return out+']';
}
inline std::string TotalsJSON(const Totals& t) {
    std::string out="{";
#define P2D_TOTAL(name) if(out.size()>1)out+=',';out+="\"" #name "\":"+std::to_string(t.name);
    P2D_TOTAL(calls) P2D_TOTAL(pixels) P2D_TOTAL(scaledPixels) P2D_TOTAL(aliasPixels)
    P2D_TOTAL(snapshotBytes) P2D_TOTAL(render) P2D_TOTAL(compute) P2D_TOTAL(blit)
    P2D_TOTAL(draws) P2D_TOTAL(creates) P2D_TOTAL(clearBytes) P2D_TOTAL(elidedInitialization)
    P2D_TOTAL(fusedInitialization) P2D_TOTAL(fastFills)
    P2D_TOTAL(sampleFailures) P2D_TOTAL(diagnosticGaps)
    P2D_TOTAL(deferredFills) P2D_TOTAL(materializedFills) P2D_TOTAL(omittedFills)
    P2D_TOTAL(actualDraws)
#undef P2D_TOTAL
    return out+",\"saturated\":"+std::to_string(t.saturated)+",\"kindPixels\":"+Numbers(t.kindPixels)+
        ",\"ends\":"+Numbers(t.ends)+",\"clears\":"+Numbers(t.clears)+'}';
}
class Recorder {
    bool enabled=false;
    uint64_t logicalSerial=0;
    std::shared_ptr<Sample> sample;
public:
    const uint64_t epoch=NextEpoch();
    Totals totals;
    uint64_t nextReportNS=0;
    void Enable(bool value) {if(enabled && !value)totals.Add(totals.diagnosticGaps);enabled=value;if(!value)sample.reset();}
    bool Enabled() const {return enabled;}
    void Begin(uint64_t command,uint64_t frame,bool sampled,uint64_t windowPixels,double drawableMS,double frameMS) {
        sample.reset();if(!enabled || !sampled)return;
        try {sample=std::make_shared<Sample>();}catch(...){totals.Add(totals.sampleFailures);return;}
        sample->epoch=epoch;sample->command=command;
        sample->firstFrame=frame;sample->windowPixels=windowPixels;
        sample->drawableMS=drawableMS;sample->frameMS=frameMS;
    }
    void Drawable(double milliseconds) {if(sample){sample->drawableMS=milliseconds;sample->drawableObserved=true;}}
    template<class F> void Count(F function) {if(!enabled)return;function(totals);if(sample)function(sample->totals);}
    int Resource(const ResourceInfo* r) {
        if(!sample || !r)return -1;
        for(size_t i=0;i<sample->resourceCount;++i)if(sample->resources[i].generation==r->generation) {
            // Labels may become available after texture creation; versions live
            // on each operation so later updates do not rewrite earlier reads.
            sample->resources[i]=*r;return int(i);
        }
        if(sample->resourceCount==ResourceCapacity){++sample->resourceOverflow;return -1;}
        sample->resources[sample->resourceCount]=*r;return int(sample->resourceCount++);
    }
    void Record(Access access,int kind,const ResourceInfo* target,const ResourceInfo* source,
                const std::array<int,4>& destination={},const std::array<int,4>& sourceRect={},
                const std::array<int,4>& clip={},uint64_t pixels=0,bool scaled=false,bool alias=false,
                bool readsTarget=false,bool full=false,int opaque=-1,uint64_t encoder=0,
                uint64_t snapshotBytes=0,int reason=0,const TVPLayerOperation* parameters=nullptr,
                unsigned sampling=0,uint64_t logicalID=0,Execution execution=Execution::Immediate) {
        if(!enabled)return;
        if(kind>0) {
            if(!logicalID)logicalID=++logicalSerial;
            else logicalSerial=std::max(logicalSerial,logicalID);
        }
        if(kind>0 && size_t(kind)<TVP_LAYER_OPERATION_COUNT)Count([&](Totals& t){
            t.Add(t.calls);t.Add(t.pixels,pixels);t.Add(t.kindPixels[size_t(kind)],pixels);
            if(scaled)t.Add(t.scaledPixels,pixels);if(alias)t.Add(t.aliasPixels,pixels);
        });
        if(snapshotBytes)Count([&](Totals& t){t.Add(t.snapshotBytes,snapshotBytes);});
        if(!sample)return;
        const int ti=Resource(target),si=Resource(source);const uint64_t sequence=++sample->sequence;
        if(sample->operationCount==OperationCapacity){++sample->operationOverflow;return;}
        auto& op=sample->operations[sample->operationCount++];const auto context=Current();
        op.receiver=context.currentReceiver;op.receiverKind=unsigned(context.receiverKind);
        op.logicalID=logicalID;op.execution=execution;op.sampling=sampling;
        if(parameters){op.flags=parameters->flags;op.color=parameters->color;op.opacity=parameters->opacity;}
        op.sequence=sequence;op.layer=context.currentLayer;CopyBounded(op.role,sizeof(op.role),context.role);
        op.access=access;op.kind=kind;op.target=ti;op.source=si;op.encoder=encoder;
        op.targetVersion=target?target->identity.version:0;op.sourceVersion=source?source->identity.version:0;
        op.destination=destination;op.sourceRect=sourceRect;op.clip=clip;op.pixels=pixels;
        op.scaled=scaled;op.alias=alias;op.readsTarget=readsTarget;op.full=full;op.opaque=opaque;
        op.snapshotBytes=snapshotBytes;op.reason=reason;
    }
    void Pass(unsigned type,const ResourceInfo* resource,bool clear,ClearOrigin origin,uint64_t encoder) {
        Count([&](Totals& t){if(type==0)t.Add(t.render);else if(type==1)t.Add(t.compute);else t.Add(t.blit);
            if(clear){t.Add(t.clears[size_t(origin)]);if(resource)t.Add(t.clearBytes,
                uint64_t(resource->width)*resource->height*resource->bytesPerPixel);}});
        Record(Access::Pass,0,resource,nullptr,{}, {}, {},0,false,false,!clear,clear,-1,encoder,0,int(origin));
    }
    void End(Boundary reason,uint64_t encoder) {
        Count([&](Totals& t){t.Add(t.ends[size_t(reason)]);});
        Record(Access::End,0,nullptr,nullptr,{}, {}, {},0,false,false,false,false,-1,encoder,0,int(reason));
    }
    std::shared_ptr<const Sample> Freeze(uint64_t lastFrame) {
        if(sample)sample->lastFrame=lastFrame;auto frozen=sample;sample.reset();return frozen;
    }
};
inline std::string ResourceJSON(const ResourceInfo& r) {
    const auto& i=r.identity;
    return '['+std::to_string(r.generation)+','+std::to_string(i.session)+','+std::to_string(i.texture)+','+
        std::to_string(i.parentSession)+','+std::to_string(i.parent)+','+std::to_string(i.creatorLayer)+','+
        std::to_string(i.assetHash)+','+std::to_string(r.width)+','+std::to_string(r.height)+','+
        std::to_string(r.bytesPerPixel)+','+std::to_string(i.assetTruncated)+','+JSON(i.asset)+','+JSON(i.role)+','+
        std::to_string(i.displayShortened)+']';
}
inline std::string OperationJSON(const Operation& o) {
    return '['+std::to_string(o.sequence)+','+std::to_string(unsigned(o.access))+','+std::to_string(o.kind)+','+
        std::to_string(o.target)+','+std::to_string(o.source)+','+std::to_string(o.targetVersion)+','+
        std::to_string(o.sourceVersion)+','+std::to_string(o.layer)+','+JSON(o.role)+','+
        std::to_string(o.encoder)+','+std::to_string(o.pixels)+','+std::to_string(o.snapshotBytes)+','+
        std::to_string(o.full)+','+std::to_string(o.opaque)+','+std::to_string(o.readsTarget)+','+
        std::to_string(o.scaled)+','+std::to_string(o.alias)+','+std::to_string(o.reason)+','+
        Numbers(o.destination)+','+Numbers(o.sourceRect)+','+Numbers(o.clip)+','+
        std::to_string(o.receiver)+','+std::to_string(o.receiverKind)+','+std::to_string(o.flags)+','+
        std::to_string(o.color)+','+std::to_string(o.opacity)+','+std::to_string(o.sampling)+','+
        std::to_string(o.logicalID)+','+std::to_string(unsigned(o.execution))+']';
}
inline std::string Base64(const std::string& input) {
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;unsigned value=0;int bits=-6;
    for(unsigned char c:input){value=(value<<8)|c;bits+=8;while(bits>=0){out+=alphabet[(value>>bits)&63];bits-=6;}}
    if(bits>-6)out+=alphabet[((value<<8)>>(bits+8))&63];while(out.size()%4)out+='=';return out;
}
inline std::vector<std::string> FormatSample(const Sample& s,double gpuMS,bool available,
                                            unsigned stageDropped,unsigned stageFailed) {
    const bool hot=(s.windowPixels && s.totals.pixels/s.windowPixels>=8) || s.totals.render>=32 ||
        (available && gpuMS>=16.7) || s.drawableMS>=8 || s.frameMS>=33.3;
    std::vector<std::string> resources,ops;
    for(size_t i=0;i<s.resourceCount;++i)resources.push_back(ResourceJSON(s.resources[i]));
    for(size_t i=0;i<s.operationCount;++i)ops.push_back(OperationJSON(s.operations[i]));
    const auto rows=[](const std::vector<std::string>& values){std::string out="[";
        for(const auto& row:values){if(out.size()>1)out+=',';out+=row;}return out+']';};
    size_t dropped=0;std::string encoded;
    // Header + base64 framing is charged against the same per-sample budget.
    for(;;){
        const std::string document="{\"version\":2,\"epoch\":"+std::to_string(s.epoch)+
            ",\"id\":"+std::to_string(s.command)+",\"firstFrame\":"+std::to_string(s.firstFrame)+
            ",\"lastFrame\":"+std::to_string(s.lastFrame)+",\"hot\":"+std::to_string(hot)+
            ",\"gpuMS\":"+std::to_string(available?gpuMS:-1)+",\"drawableMS\":"+std::to_string(s.drawableMS)+
            ",\"drawableObserved\":"+std::to_string(s.drawableObserved)+
            ",\"frameMS\":"+std::to_string(s.frameMS)+",\"stageDropped\":"+std::to_string(stageDropped)+
            ",\"stageFailed\":"+std::to_string(stageFailed)+",\"operationOverflow\":"+std::to_string(s.operationOverflow)+
            ",\"resourceOverflow\":"+std::to_string(s.resourceOverflow)+",\"outputDropped\":"+std::to_string(dropped)+
            ",\"totals\":"+TotalsJSON(s.totals)+",\"resources\":"+rows(resources)+",\"operations\":"+rows(ops)+'}';
        encoded=Base64(document);
        const size_t parts=(encoded.size()+ChunkBytes-1)/ChunkBytes;
        if(encoded.size()+parts*180<=OutputBytes-2048)break;
        if(!ops.empty())ops.pop_back();else if(!resources.empty())resources.pop_back();else return {};
        ++dropped;
    }
    std::vector<std::string> out;const size_t parts=(encoded.size()+ChunkBytes-1)/ChunkBytes;
    for(size_t p=0;p<parts;++p)out.push_back("metal.layerHotspot version=2 epoch="+std::to_string(s.epoch)+
        " id="+std::to_string(s.command)+" part="+std::to_string(p)+" parts="+std::to_string(parts)+
        " data="+encoded.substr(p*ChunkBytes,ChunkBytes));
    return out;
}
// Shared solely with completion callbacks; serializes a strict rolling-second
// byte budget across cumulative reports and asynchronously completed samples.
struct OutputGate {
    struct Emission {uint64_t time=0;size_t bytes=0;};
    std::mutex mutex;size_t bytes=0,emissionCount=0;uint64_t droppedReports=0;
    std::array<Emission,64> emissions{};
    void Drop() {std::lock_guard<std::mutex> lock(mutex);++droppedReports;}
    template<class Log> void Emit(uint64_t now,const std::vector<std::string>& lines,Log log) {
        std::lock_guard<std::mutex> lock(mutex);
        while(emissionCount && now>=emissions[0].time && now-emissions[0].time>=1000000000ULL) {
            bytes-=emissions[0].bytes;
            for(size_t i=1;i<emissionCount;++i)emissions[i-1]=emissions[i];
            --emissionCount;
        }
        size_t size=0;for(const auto& line:lines){if(line.size()>MessageBytes){++droppedReports;return;}size+=line.size()+1;}
        if(size>OutputBytes-bytes || emissionCount==emissions.size()){++droppedReports;return;}
        for(const auto& line:lines)log(line.c_str());bytes+=size;
        emissions[emissionCount++]={now,size};
    }
    template<class Log> void Report(Recorder& recorder,uint64_t now,uint64_t command,bool final,Log log) {
        if(!recorder.Enabled() || (!final && now<recorder.nextReportNS))return;
        recorder.nextReportNS=now+1000000000ULL;
        std::string document=TotalsJSON(recorder.totals);
        // Separate aggregate chunks are atomic as a group and explicitly final.
        const auto encoded=Base64(document);std::vector<std::string> lines;
        const size_t parts=(encoded.size()+ChunkBytes-1)/ChunkBytes;
        uint64_t lost=0;{std::lock_guard<std::mutex> lock(mutex);lost=droppedReports;}
        for(size_t p=0;p<parts;++p)lines.push_back("metal.layerPasses version=2 epoch="+std::to_string(recorder.epoch)+
            " id="+std::to_string(command)+" final="+std::to_string(final)+" droppedReports="+std::to_string(lost)+
            " part="+std::to_string(p)+" parts="+std::to_string(parts)+" data="+encoded.substr(p*ChunkBytes,ChunkBytes));
        Emit(now,lines,log);
    }
};
} }
