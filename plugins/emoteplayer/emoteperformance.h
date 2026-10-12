#pragma once

#include <atomic>
#include <cstdint>

namespace emoteplayer {
// Session counters for Emote canvas capture and synchronous UI alpha reads.
// The opt-in caching/async-alpha experiments that used to live here were
// removed after they cost more than they saved; see EMOTE-PERFORMANCE-STATUS.md.

#define EMOTE_PERFORMANCE_COUNTERS(X) \
    X(captureRequests) X(captureFull) X(captureFullCopies) X(captureFullPixels) \
    X(captureUpdatePixels) X(captureUpdateFullPixels) X(uiSyncReads) X(uiSyncWaitNS)

struct EmotePerformanceStats {
#define EMOTE_VALUE(name) std::uint64_t name = 0;
    EMOTE_PERFORMANCE_COUNTERS(EMOTE_VALUE)
#undef EMOTE_VALUE
};
struct EmotePerformanceCounters {
#define EMOTE_ATOMIC(name) std::atomic<std::uint64_t> name{0};
    EMOTE_PERFORMANCE_COUNTERS(EMOTE_ATOMIC)
#undef EMOTE_ATOMIC
};
inline EmotePerformanceCounters& performanceCounters()
{
    static EmotePerformanceCounters counters;
    return counters;
}
inline EmotePerformanceStats performanceStats()
{
    EmotePerformanceStats snapshot;
#define EMOTE_READ(name) snapshot.name = performanceCounters().name.load(std::memory_order_relaxed);
    EMOTE_PERFORMANCE_COUNTERS(EMOTE_READ)
#undef EMOTE_READ
    return snapshot;
}
inline void resetPerformanceStats()
{
#define EMOTE_RESET(name) performanceCounters().name.store(0, std::memory_order_relaxed);
    EMOTE_PERFORMANCE_COUNTERS(EMOTE_RESET)
#undef EMOTE_RESET
}
#undef EMOTE_PERFORMANCE_COUNTERS
} // namespace emoteplayer
