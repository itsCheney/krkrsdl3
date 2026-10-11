#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

// Shared by the native uploader and portable ownership/bounds tests. These
// allocators never wrap: a command buffer's bytes remain immutable until retired.
namespace krkrsdl3::layer_upload {
constexpr int AtlasSide = 1024;
constexpr size_t AtlasBudget = 16 * 1024 * 1024;
constexpr size_t TinyBytes = 64 * 1024;
constexpr size_t ArenaBytes = 1024 * 1024;
constexpr size_t ArenaBudget = 16 * 1024 * 1024;
struct AtlasLayout {
    int x=0,y=0,rowHeight=0;
    bool Allocate(int width,int height,int& left,int& top) {
        if(width<=0 || height<=0 || width>AtlasSide || height>AtlasSide) return false;
        int nextX=x,nextY=y,nextHeight=rowHeight;
        if(nextX>AtlasSide-width) {nextX=0;nextY+=nextHeight;nextHeight=0;}
        if(nextY>AtlasSide-height) return false;
        left=nextX;top=nextY;x=nextX+width;y=nextY;
        rowHeight=height>nextHeight ? height : nextHeight;return true;
    }
};
struct ArenaLayout {
    size_t offset=0;
    bool Allocate(size_t bytes,size_t& result) {
        if(!bytes || bytes>ArenaBytes || offset>ArenaBytes-255) return false;
        const size_t aligned=(offset+255)&~size_t(255);
        if(aligned>ArenaBytes-bytes) return false;
        result=aligned;offset=aligned+bytes;return true;
    }
};
inline bool UploadSize(int width,int height,int bytesPerPixel,size_t& row,size_t& bytes) {
    if(width<=0 || height<=0 || (bytesPerPixel!=1 && bytesPerPixel!=4)) return false;
    if(size_t(width)>(std::numeric_limits<size_t>::max()-255)/bytesPerPixel) return false;
    row=(size_t(width)*bytesPerPixel+255)&~size_t(255);
    if(size_t(height)>std::numeric_limits<size_t>::max()/row) return false;
    bytes=row*size_t(height);return true;
}
// A page may be appended only before its owner is submitted. Completion is
// necessary but an outstanding native caller also prevents resetting the page.
inline bool CanAppend(uint64_t owner,uint64_t current,bool sealed) {
    return owner==current && !sealed;
}
inline bool CanRecycle(bool retired,unsigned reservations) {
    return retired && reservations==0;
}
enum class GlyphReject : unsigned { Domain, Target, Opacity, Size, Tables, Shared, Capacity, Backend, Count };
struct Counters {
    uint64_t glyphCalls=0,glyphApplied=0,glyphBytes=0,glyphPrepNS=0,glyphEncodeNS=0;
    uint64_t glyphRejected[unsigned(GlyphReject::Count)]{};
    uint64_t uploadCalls=0,uploadBytes=0,uploadBatches=0,arenaPages=0,arenaFallbacks=0;
    uint64_t uploadPrepNS=0,uploadEncodeNS=0,atlasPeakBytes=0;
    uint64_t flushRender=0,flushCompute=0,flushBlit=0,flushSubmit=0,flushDestroy=0;
};
}
