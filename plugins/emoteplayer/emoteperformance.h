#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>

namespace emoteplayer {
// Experimental optimizations are opt-in until the Apple A/B acceptance run.
// SDL hints take precedence over environment variables, including an explicit 0.
inline bool performanceEnabled(const char* name)
{
    const char* value = SDL_GetHint(name);
    if (!value) value = std::getenv(name);
    return value && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
                     std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0);
}

#define EMOTE_PERFORMANCE_COUNTERS(X) \
    X(nodeVisits) X(nodeCacheHits) X(nodeCacheMisses) X(localPoseCacheHits) X(localPoseCacheMisses) X(shapeCacheHits) X(meshCacheHits) \
    X(drawListRebuilds) X(captureRequests) X(captureSkips) X(captureFull) X(captureRegions) \
    X(captureFullCopies) X(captureRegionCopies) X(captureInvalidDestinations) X(captureRegionFallbacks) \
    X(captureKnownBounds) X(captureUnknownBounds) X(captureRegionPixels) X(captureFullPixels) \
    X(captureBoundsNS) \
    X(captureUpdatePixels) X(captureUpdateFullPixels) \
    X(captureExperimentalBoundsKnown) X(captureExperimentalBoundsUnknown) \
    X(captureExperimentalBoundsPixels) X(captureExperimentalBoundsNS) \
    X(captureCOWFallbacks) X(alphaRequests) X(alphaCacheHits) X(alphaPendingEvents) \
    X(alphaReadBytes) X(alphaFailures) X(uiSyncReads) X(uiSyncWaitNS)

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
