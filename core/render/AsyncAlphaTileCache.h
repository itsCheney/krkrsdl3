#pragma once
#include "AsyncLayerReadback.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <limits>

namespace krkrsdl3 {
struct AsyncAlphaTileSnapshot {
    uint64_t version = 0;
    std::shared_ptr<AsyncLayerReadback> read;
    std::shared_ptr<AsyncLayerPresentation> presentation;
    std::vector<uint8_t> alpha;
    std::shared_ptr<AsyncLayerPresentation> Ticket() const {
        return presentation ? presentation : read ? read->presentation : nullptr;
    }
    bool Materialize() {
        if(!alpha.empty()) return presentation &&
            presentation->presented.load(std::memory_order_acquire) &&
            !presentation->failed.load(std::memory_order_acquire);
        if(!read || !read->Ready()) return false;
        const int w=read->region.Width(), h=read->region.Height();
        if(w<=0 || h<=0 || read->pitch<w*4 ||
           read->rgba.size()<size_t(read->pitch)*h) return false;
        alpha.resize(size_t(w)*h);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x)
            alpha[size_t(y)*w+x]=read->rgba[size_t(y)*read->pitch+size_t(x)*4+3];
        // The immutable R8 snapshot owns no pooled GPU storage.
        presentation=read->presentation;
        read->ReleaseStorage();
        read.reset(); // Release its allocation lease after publishing compact R8.
        return true;
    }
};
struct AsyncAlphaTile {
    TVPLayerRect region{};
    uint64_t lastUse = 0, lastRequestedVersion = std::numeric_limits<uint64_t>::max();
    bool demanded = false;
    std::deque<std::shared_ptr<AsyncAlphaTileSnapshot>> frames;
    std::shared_ptr<AsyncAlphaTileSnapshot> Latest() {
        for(auto it=frames.rbegin();it!=frames.rend();++it)
            if((*it)->Materialize()) return *it;
        return {};
    }
    std::shared_ptr<AsyncAlphaTileSnapshot> AtFrame(uint64_t serial) {
        for(auto it=frames.rbegin();it!=frames.rend();++it) {
            auto& frame=*it;
            if(frame->Materialize() && frame->presentation && frame->presentation->frameSerial==serial)
                return frame;
        }
        return {};
    }
    // An input event pins the request before GPU completion. Later cache
    // retirement must not discard the exact display frame it is waiting for.
    std::shared_ptr<AsyncAlphaTileSnapshot> RequestAtFrame(uint64_t serial) const {
        for(auto it=frames.rbegin();it!=frames.rend();++it) {
            const auto ticket=(*it)->Ticket();
            if(ticket && ticket->frameSerial==serial) return *it;
        }
        return {};
    }
    bool Sample(int x,int y,uint8_t& alpha,std::shared_ptr<AsyncAlphaTileSnapshot>& snapshot) {
        snapshot=Latest();
        if(!snapshot || x<region.left || y<region.top || x>=region.right || y>=region.bottom) return false;
        alpha=snapshot->alpha[size_t(y-region.top)*region.Width()+x-region.left];
        return true;
    }
};
// Render-thread only. Independent request state survives texture destruction;
// no result callback owns a Layer, player, or texture wrapper pointer.
class AsyncAlphaTileCache {
public:
    static constexpr int TileSize=32;
    static constexpr size_t MaxTiles=32, MaxFramesPerTile=3;
private:
    uint64_t useSerial=0;
    std::vector<std::shared_ptr<AsyncAlphaTile>> tiles;
public:
    ~AsyncAlphaTileCache() { Cancel(); }
    void Cancel() {
        for(auto& tile:tiles) for(auto& frame:tile->frames) {
            if(frame->read) {
                frame->read->canceled.store(true,std::memory_order_release);
                frame->read->ReleaseStorage();
            }
        }
    }
    std::shared_ptr<AsyncAlphaTile> Demand(int x,int y,int width,int height) {
        if(x<0 || y<0 || x>=width || y>=height) return {};
        const int left=x/TileSize*TileSize, top=y/TileSize*TileSize;
        for(auto& tile:tiles) if(tile->region.left==left && tile->region.top==top) {
            tile->lastUse=++useSerial; tile->demanded=true; return tile;
        }
        if(tiles.size()==MaxTiles) {
            auto victim=tiles.end();
            for(auto it=tiles.begin();it!=tiles.end();++it)
                if(it->use_count()==1 && (victim==tiles.end() || (*it)->lastUse<(*victim)->lastUse)) victim=it;
            if(victim==tiles.end()) return {}; // Pending events pin their tiles.
            tiles.erase(victim);
        }
        auto tile=std::make_shared<AsyncAlphaTile>();
        tile->region={left,top,std::min(left+TileSize,width),std::min(top+TileSize,height)};
        tile->lastUse=++useSerial; tile->demanded=true; tiles.push_back(tile); return tile;
    }
    template<class Encode> void EncodeDemanded(uint64_t version,
        const std::shared_ptr<AsyncLayerPresentation>& presentation,Encode encode) {
        if(!presentation) return;
        for(auto& tile:tiles) {
            // Retirement is based on consumed request state, not just GPU status.
            for(auto it=tile->frames.begin();it!=tile->frames.end();) {
                const auto& read=(*it)->read;
                if(read && read->completed.load(std::memory_order_acquire) &&
                   (read->failed.load(std::memory_order_acquire) || read->canceled.load(std::memory_order_acquire) ||
                    (read->presentation && read->presentation->failed.load(std::memory_order_acquire))))
                {
                    read->ReleaseStorage();
                    it=tile->frames.erase(it);
                }
                else ++it;
            }
            if(!tile->demanded) continue;
            auto latest=tile->Latest();
            bool pending=false;
            for(const auto& frame:tile->frames)
                if((frame->presentation && frame->presentation->frameSerial==presentation->frameSerial) ||
                   (frame->read && frame->read->presentation==presentation &&
                    !frame->read->failed.load(std::memory_order_acquire) &&
                    !frame->read->canceled.load(std::memory_order_acquire))) { pending=true; break; }
            if(pending) continue;
            while(tile->frames.size()>=MaxFramesPerTile) {
                if(tile->frames.front()->read && !tile->frames.front()->read->completed.load(std::memory_order_acquire)) break;
                tile->frames.pop_front();
            }
            if(tile->frames.size()>=MaxFramesPerTile) continue;
            auto frame=std::make_shared<AsyncAlphaTileSnapshot>();
            frame->version=version;
            // Static pixels still need a ticket for THIS composed display
            // frame, so moving and static layers can share one input snapshot.
            if(latest && latest->version==version) {
                frame->alpha=latest->alpha; frame->presentation=presentation;
                tile->frames.push_back(std::move(frame));
                continue;
            }
            frame->read=std::make_shared<AsyncLayerReadback>();
            frame->read->region=tile->region; frame->read->presentation=presentation;
            if(encode(frame->read)) tile->frames.push_back(std::move(frame));
        }
    }
    void StopDemand() { for(auto& tile:tiles) tile->demanded=false; }
    size_t TileCount() const { return tiles.size(); }
};
}
