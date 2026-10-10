#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace krkrsdl3 { namespace static_presentation {
// Content identity is independent of optional diagnostic tags. Resource IDs
// never repeat, including allocator address reuse and new game sessions.
inline uint64_t NextResourceID() noexcept {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
struct Window {
    uint64_t resource = 0, mutation = 0;
    int sourceWidth = 0, sourceHeight = 0;
    float x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Window& b) const {
        return resource==b.resource && mutation==b.mutation &&
            sourceWidth==b.sourceWidth && sourceHeight==b.sourceHeight &&
            x==b.x && y==b.y && width==b.width && height==b.height;
    }
};
struct Signature {
    int width=0,height=0;
    uint64_t epoch=0;
    std::vector<Window> windows;
    bool Valid() const {
        if(width<=0 || height<=0) return false;
        for(const auto& w:windows)
            if(!w.resource || w.sourceWidth<=0 || w.sourceHeight<=0 ||
               !std::isfinite(w.x) || !std::isfinite(w.y) ||
               !std::isfinite(w.width) || !std::isfinite(w.height) ||
               w.width<=0 || w.height<=0) return false;
        return true;
    }
    bool operator==(const Signature& b) const {
        return width==b.width && height==b.height && epoch==b.epoch && windows==b.windows;
    }
};
enum class Denial : unsigned { Disabled, Unsupported, Hidden, Invalid, Ticket,
    Refresh, Capacity, InFlight, Unconfirmed, Changed, Count };
struct Counters {
    uint64_t eligible=0, skipped=0;
    std::array<uint64_t,static_cast<unsigned>(Denial::Count)> denied{};
};
// Callback state owns metadata only. A late callback can touch its Record
// after backend destruction without capturing an Impl or retaining textures.
struct Record {
    Signature signature;
    uint64_t serial=0;
    std::atomic<bool> commandSucceeded{false}, displayed{false}, failed{false};
};
class Policy {
    static constexpr size_t Capacity=16;
    uint64_t epoch=1,nextSerial=1,confirmedSerial=0;
    bool forcedRefresh=true, hasConfirmed=false, saturated=false;
    Signature confirmed;
    std::vector<std::shared_ptr<Record>> pending;
    Counters counters;
    bool Deny(Denial reason) { ++counters.denied[static_cast<unsigned>(reason)]; return false; }
    void Reconcile() {
        for(auto it=pending.begin();it!=pending.end();) {
            const auto& record=*it;
            if(record->failed.load(std::memory_order_acquire)) {
                forcedRefresh=true; hasConfirmed=false;
                it=pending.erase(it);
            } else if(record->commandSucceeded.load(std::memory_order_acquire) &&
                      record->displayed.load(std::memory_order_acquire)) {
                if(record->signature.epoch==epoch && record->serial>confirmedSerial) {
                    confirmed=record->signature; confirmedSerial=record->serial;
                    hasConfirmed=true; forcedRefresh=false;
                }
                it=pending.erase(it);
            } else ++it;
        }
    }
public:
    uint64_t Epoch() const noexcept { return epoch; }
    void Invalidate() noexcept {
        ++epoch; forcedRefresh=true; hasConfirmed=false; saturated=false; pending.clear();
    }
    bool CanSkip(const Signature& signature,bool enabled,bool supported,
                 bool hidden,bool hasPresentationTicket) {
        Reconcile();
        if(!enabled) return Deny(Denial::Disabled);
        if(!supported) return Deny(Denial::Unsupported);
        if(hidden) return Deny(Denial::Hidden);
        if(!signature.Valid() || signature.epoch!=epoch) return Deny(Denial::Invalid);
        ++counters.eligible;
        if(hasPresentationTicket) return Deny(Denial::Ticket);
        if(saturated || pending.size()>=Capacity) return Deny(Denial::Capacity);
        // A confirmed A followed by pending B must not skip a new A.
        if(!pending.empty()) return Deny(Denial::InFlight);
        if(forcedRefresh) return Deny(Denial::Refresh);
        if(!hasConfirmed) return Deny(Denial::Unconfirmed);
        if(!(signature==confirmed)) return Deny(Denial::Changed);
        ++counters.skipped; return true;
    }
    std::shared_ptr<Record> Track(Signature signature) {
        Reconcile();
        if(saturated || pending.size()>=Capacity || signature.epoch!=epoch) {
            // Untracked presents can supersede the known surface. Remain
            // conservative until a lifecycle invalidation starts a new epoch.
            forcedRefresh=true; hasConfirmed=false; saturated=true; return {};
        }
        auto record=std::make_shared<Record>();
        record->signature=std::move(signature); record->serial=nextSerial++;
        pending.push_back(record); return record;
    }
    const Counters& Stats() const noexcept { return counters; }
    size_t PendingCount() const noexcept { return pending.size(); }
};
} }
