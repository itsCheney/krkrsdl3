#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>
#include <utility>

// Decoded payloads only; compressed frames remain in the source stream. One
// oversized frame may exceed the budget so valid large canvases still play.
class AMVDecodedFrameCache {
    struct Entry { size_t frame; std::vector<uint8_t> pixels; };
    std::list<Entry> entries;
    size_t bytes=0;
public:
    static constexpr size_t Budget=32u*1024u*1024u;
    static constexpr size_t MaxEntries=16;
    const std::vector<uint8_t>* Find(size_t frame) {
        for(auto it=entries.begin();it!=entries.end();++it) if(it->frame==frame) {
            entries.splice(entries.begin(),entries,it); return &entries.front().pixels;
        }
        return nullptr;
    }
    const std::vector<uint8_t>& Put(size_t frame,std::vector<uint8_t> pixels) {
        for(auto it=entries.begin();it!=entries.end();++it) if(it->frame==frame) {
            bytes-=it->pixels.size(); entries.erase(it); break;
        }
        while(!entries.empty() && (pixels.size()>Budget || bytes>Budget-pixels.size() || entries.size()>=MaxEntries)) {
            bytes-=entries.back().pixels.size(); entries.pop_back();
        }
        bytes+=pixels.size(); entries.push_front({frame,std::move(pixels)});
        return entries.front().pixels;
    }
    void Clear() { entries.clear(); bytes=0; }
    size_t Bytes() const { return bytes; }
    size_t Count() const { return entries.size(); }
};
