#pragma once
#include <atomic>
#include <cstdint>
namespace krkrsdl3 { namespace transition_cache {
// The session initializer may opt out before any Layer transition begins.
inline std::atomic<bool>& EnabledStorage() { static std::atomic<bool> value{true}; return value; }
inline void SetEnabled(bool enabled) { EnabledStorage().store(enabled,std::memory_order_relaxed); }
inline bool Enabled() { return EnabledStorage().load(std::memory_order_relaxed); }
struct Counters { uint64_t eligible=0,materialized=0,cancelled=0; };
struct AtomicCounters { std::atomic<uint64_t> eligible{0},materialized{0},cancelled{0}; };
inline AtomicCounters& Storage() { static AtomicCounters counters; return counters; }
inline Counters Snapshot() {
    auto& s=Storage(); return {s.eligible.load(std::memory_order_relaxed),
        s.materialized.load(std::memory_order_relaxed),s.cancelled.load(std::memory_order_relaxed)};
}
} }
