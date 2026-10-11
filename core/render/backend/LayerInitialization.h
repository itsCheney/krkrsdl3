#pragma once

#include <cstdint>

namespace krkrsdl3::layer_initialization {

// A newly allocated Layer has logical zero pixels. The physical clear can be
// deferred until consumption, or replaced by a proven full-surface write.
// Queries never mutate state: callers commit only after encoding succeeds.
class State {
    bool pending_ = false;
public:
    explicit State(bool pending = false) : pending_(pending) {}
    bool Pending() const { return pending_; }
    bool NeedsClearBeforeWrite(bool fullOverwrite) const { return pending_ && !fullOverwrite; }
    void EncodedClear() { pending_ = false; }
    void EncodedWrite(bool fullOverwrite) { if(fullOverwrite) pending_ = false; }
};

inline bool FullSurface(int width, int height, int left, int top, int right, int bottom) {
    return width > 0 && height > 0 && left == 0 && top == 0 && right == width && bottom == height;
}

// FillColor/FillMask/blended fills must retain their byte arithmetic shader.
// A running pass also retains its draw, avoiding an otherwise needless store.
inline bool CanLoadClearFill(bool fill, uint32_t flags, bool rgba8, bool activeSameTarget,
                             int width, int height, int left, int top, int right, int bottom) {
    return fill && flags == 0 && rgba8 && !activeSameTarget &&
        FullSurface(width,height,left,top,right,bottom);
}

inline double ClearChannel(uint32_t color, unsigned channel) {
    return double((color >> (channel * 8)) & 255u) / 255.0;
}

} // namespace krkrsdl3::layer_initialization
