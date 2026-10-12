#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>

namespace emoteplayer {
// Experimental optimizations are opt-in until the Apple A/B acceptance run.
// SDL hints take precedence over environment variables, including an explicit 0.
inline bool readPerformanceFlag(const char* name)
{
    const char* value = SDL_GetHint(name);
    if (!value) value = std::getenv(name);
    return value && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
                     std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0);
}

// Same order as the host's per-launch options mask. Bits 5 (experimental
// bounds) and 6 (local pose cache) were removed and stay reserved.
inline constexpr const char* performanceFlagNames[] = {
    "MIKAGE_EMOTE_NODE_CACHE", "MIKAGE_EMOTE_CAPTURE_CACHE", "MIKAGE_EMOTE_LOCAL_UPDATE",
    "MIKAGE_EMOTE_REGION_COPY", "MIKAGE_EMOTE_ASYNC_ALPHA"};
// Bit 31 marks a valid snapshot.
inline std::atomic<std::uint32_t>& performanceFlagSnapshot()
{
    static std::atomic<std::uint32_t> snapshot{0};
    return snapshot;
}
// The host sets these hints once per launch, before the game starts, then
// calls this. Each SDL_GetHint takes SDL's hint lock and getenv scans the
// environment; the draw and composition paths asked a dozen times per frame.
inline void refreshPerformanceFlags()
{
    std::uint32_t bits = 1u << 31;
    for (std::uint32_t i = 0; i < sizeof(performanceFlagNames) / sizeof(*performanceFlagNames); ++i)
        if (readPerformanceFlag(performanceFlagNames[i])) bits |= 1u << i;
    performanceFlagSnapshot().store(bits, std::memory_order_release);
}
inline bool performanceEnabled(const char* name)
{
    const std::uint32_t bits = performanceFlagSnapshot().load(std::memory_order_acquire);
    if (bits & (1u << 31))
        for (std::uint32_t i = 0; i < sizeof(performanceFlagNames) / sizeof(*performanceFlagNames); ++i)
            if (std::strcmp(name, performanceFlagNames[i]) == 0) return (bits >> i) & 1u;
    // No snapshot yet (tests, other hosts) or an unknown name: read live.
    return readPerformanceFlag(name);
}

#define EMOTE_PERFORMANCE_COUNTERS(X) \
    X(nodeVisits) X(nodeCacheHits) X(nodeCacheMisses) X(shapeCacheHits) X(meshCacheHits) \
    X(drawListRebuilds) X(captureRequests) X(captureSkips) X(captureFull) X(captureRegions) \
    X(captureFullCopies) X(captureRegionCopies) X(captureInvalidDestinations) X(captureRegionFallbacks) \
    X(captureKnownBounds) X(captureUnknownBounds) X(captureRegionPixels) X(captureFullPixels) \
    X(captureBoundsNS) \
    X(captureUpdatePixels) X(captureUpdateFullPixels) \
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
