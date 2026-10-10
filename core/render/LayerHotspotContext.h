#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Metadata only: these values never own Layers, textures, or backend resources.
namespace krkrsdl3 { namespace layer_hotspot {
struct Identity {
    uint64_t session = 0, texture = 0, version = 0;
    uint64_t parent = 0, parentSession = 0, creatorLayer = 0;
    uint64_t assetHash = 0;
    char asset[192] = {}, role[24] = {};
    bool assetTruncated = false;
};
struct Context {
    uint64_t currentLayer = 0;
    char role[24] = {};
};
inline void CopyBounded(char* target, size_t capacity, const char* source) noexcept {
    if(!capacity) return;
    if(!source) source = "";
    size_t count = std::strlen(source);
    if(count >= capacity) {
        count = capacity - 1;
        // Do not split a UTF-8 codepoint at the buffer boundary.
        while(count && (static_cast<unsigned char>(source[count]) & 0xc0) == 0x80) --count;
    }
    std::memcpy(target, source, count);
    target[count] = 0;
}
inline Context& CurrentStorage() noexcept {
    static thread_local Context context;
    return context;
}
inline Context Current() noexcept { return CurrentStorage(); }
inline uint64_t NextLayerID() noexcept {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
class Scope {
    Context previous;
public:
    Scope(uint64_t layer, const char* role) noexcept : previous(CurrentStorage()) {
        CurrentStorage().currentLayer = layer;
        CopyBounded(CurrentStorage().role, sizeof(CurrentStorage().role), role);
    }
    ~Scope() { CurrentStorage() = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
// Hash the complete normalized storage path, even when the displayed path is
// truncated. Slash normalization keeps cache-hit and decoded-image tags equal.
inline void SetAsset(Identity& identity, const char* name) noexcept {
    if(!name) name = "";
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t length = 0;
    for(const char* p = name; *p; ++p, ++length) {
        const unsigned char ch = *p == '\\' ? '/' : static_cast<unsigned char>(*p);
        hash = (hash ^ ch) * UINT64_C(1099511628211);
    }
    CopyBounded(identity.asset, sizeof(identity.asset), name);
    for(char* p = identity.asset; *p; ++p) if(*p == '\\') *p = '/';
    identity.assetHash = length ? hash : 0;
    identity.assetTruncated = length >= sizeof(identity.asset);
}
inline Identity CreatedIdentity(uint64_t session, uint64_t texture) noexcept {
    Identity result;
    const auto current = Current();
    result.session = session; result.texture = texture;
    result.creatorLayer = current.currentLayer;
    CopyBounded(result.role, sizeof(result.role), current.role[0] ? current.role : "texture");
    return result;
}
inline void SetParent(Identity& identity, const Identity& parent, const char* role,
                      bool copiedContent) noexcept {
    identity.parent = parent.texture; identity.parentSession = parent.session;
    CopyBounded(identity.role, sizeof(identity.role), role);
    if(copiedContent) {
        std::memcpy(identity.asset, parent.asset, sizeof(identity.asset));
        identity.assetHash = parent.assetHash; identity.assetTruncated = parent.assetTruncated;
    } else {
        identity.asset[0] = 0; identity.assetHash = 0; identity.assetTruncated = false;
    }
}
} }
