#pragma once

#include <atomic>
#include <cstdint>

// Diagnostic context only. No render state, resource ownership or script values
// are retained here. Scoped contexts survive nested reads and restore on throw.
namespace krkrsdl3::point_trace
{
enum class Source { Unknown, LayerHitTest, LayerMask, LayerColor, BitmapMask, BitmapColor };
enum class Trigger { Unknown, PointerMove, PointerDown, PointerUp, Click, DoubleClick, Wheel,
                     InputRecheck, ScriptHitTest, CursorChange, HintChange };
enum class Invalidation { Unknown, CPUWrite, CPUUpload, GPUUpdate, GPUOperation,
                          GPUOverwrite, Explicit };

inline const char* Name(Source source) {
    switch (source) {
        case Source::LayerHitTest: return "layer.hitTest";
        case Source::LayerMask: return "layer.getMaskPixel";
        case Source::LayerColor: return "layer.getMainPixel";
        case Source::BitmapMask: return "bitmap.getMaskPixel";
        case Source::BitmapColor: return "bitmap.getPixel";
        default: return "unknown";
    }
}
inline const char* Name(Trigger trigger) {
    switch (trigger) {
        case Trigger::PointerMove: return "pointerMove";
        case Trigger::PointerDown: return "pointerDown";
        case Trigger::PointerUp: return "pointerUp";
        case Trigger::Click: return "click";
        case Trigger::DoubleClick: return "doubleClick";
        case Trigger::Wheel: return "wheel";
        case Trigger::InputRecheck: return "inputRecheck";
        case Trigger::ScriptHitTest: return "scriptHitTest";
        case Trigger::CursorChange: return "cursorChange";
        case Trigger::HintChange: return "hintChange";
        default: return "unknown";
    }
}
inline const char* Name(Invalidation reason) {
    switch (reason) {
        case Invalidation::CPUWrite: return "cpuWrite";
        case Invalidation::CPUUpload: return "cpuUpload";
        case Invalidation::GPUUpdate: return "gpuUpdate";
        case Invalidation::GPUOperation: return "gpuOperation";
        case Invalidation::GPUOverwrite: return "gpuOverwrite";
        case Invalidation::Explicit: return "explicitInvalidate";
        default: return "unknown";
    }
}
struct Origin {
    Source source = Source::Unknown;
    Trigger trigger = Trigger::Unknown, parentTrigger = Trigger::Unknown;
    std::uintptr_t owner = 0;
};
struct Query {
    std::uint64_t queryID = 0, textureID = 0, version = 0;
    Origin origin;
    int x = 0, y = 0, width = 0, height = 0;
    bool alphaOnly = false;
    const char* missReason = "notCached";
    Invalidation invalidation = Invalidation::Unknown;
    const char* writer = "unknown";
    std::uint64_t invalidatedVersion = 0;
    Invalidation lastInvalidation = Invalidation::Unknown;
    const char* lastWriter = "initial";
    int lastWriteLeft = 0, lastWriteTop = 0, lastWriteRight = 0, lastWriteBottom = 0;
    bool reported = false;
    std::uint64_t lastSubmittedID = 0, renderFrame = 0;
    std::uint64_t wallNS = 0, gpuWaitNS = 0, finishedNS = 0;
};

inline std::atomic<bool> enabled{false};
inline std::atomic<std::uint64_t> nextQueryID{0}, nextTextureID{0};
inline thread_local Origin origin;
inline thread_local Query* query = nullptr;
inline thread_local const char* writer = nullptr;
inline bool Enabled() { return enabled.load(std::memory_order_relaxed); }
inline void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }
inline Query* CurrentQuery() { return Enabled() ? query : nullptr; }
inline const char* CurrentWriter() { return Enabled() ? writer : nullptr; }
inline std::uint64_t NextTextureID() { return nextTextureID.fetch_add(1, std::memory_order_relaxed) + 1; }

class OriginScope {
    Origin previous;
    bool active;
public:
    OriginScope(Source source, const void* owner) : previous(origin), active(Enabled()) {
        if (active) { origin.source = source; origin.owner = reinterpret_cast<std::uintptr_t>(owner); }
    }
    ~OriginScope() { if (active) origin = previous; }
    OriginScope(const OriginScope&) = delete;
    OriginScope& operator=(const OriginScope&) = delete;
};
class TriggerScope {
    Origin previous;
    bool active;
public:
    explicit TriggerScope(Trigger value, bool preserveExisting = true) : previous(origin), active(Enabled()) {
        if (active && (!preserveExisting || origin.trigger == Trigger::Unknown)) {
            origin.parentTrigger = origin.trigger;
            origin.trigger = value;
        }
    }
    ~TriggerScope() { if (active) origin = previous; }
    TriggerScope(const TriggerScope&) = delete;
    TriggerScope& operator=(const TriggerScope&) = delete;
};
class WriterScope {
    const char* previous;
    bool active;
public:
    // Values are retained in texture diagnostic metadata: use static labels.
    explicit WriterScope(const char* value) : previous(writer), active(Enabled()) { if (active) writer = value; }
    ~WriterScope() { if (active) writer = previous; }
    WriterScope(const WriterScope&) = delete;
    WriterScope& operator=(const WriterScope&) = delete;
};
class QueryScope {
    Query* previous;
    bool active;
public:
    explicit QueryScope(Query& value) : previous(query), active(Enabled()) {
        if (active) {
            value.queryID = nextQueryID.fetch_add(1, std::memory_order_relaxed) + 1;
            value.origin = origin;
            query = &value;
        }
    }
    ~QueryScope() { if (active) query = previous; }
    QueryScope(const QueryScope&) = delete;
    QueryScope& operator=(const QueryScope&) = delete;
};
}
