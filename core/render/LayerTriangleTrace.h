#pragma once
#include <atomic>

// Attribution only: no RenderManager API changes or retained script objects.
// The host enables capture with its diagnostic log; callers restore their tag
// on nested operations and exception unwind.
namespace krkrsdl3::layer_triangle_trace {
enum class Source { Unknown, AffineCopy, AffinePile, AffineBlend, OperateAffine, Count };
inline const char* Name(Source source) {
    switch(source) {
        case Source::AffineCopy: return "AffineCopy";
        case Source::AffinePile: return "AffinePile";
        case Source::AffineBlend: return "AffineBlend";
        case Source::OperateAffine: return "OperateAffine";
        default: return "unknown";
    }
}
inline std::atomic<bool> enabled{false};
inline thread_local Source source = Source::Unknown;
inline bool Enabled() { return enabled.load(std::memory_order_relaxed); }
class SourceScope {
    Source previous;
    bool active;
public:
    explicit SourceScope(Source value) : previous(source), active(Enabled()) {
        if(active) source=value;
    }
    ~SourceScope() { if(active) source=previous; }
    SourceScope(const SourceScope&) = delete;
    SourceScope& operator=(const SourceScope&) = delete;
};
}
