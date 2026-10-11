#pragma once
#include "ComplexRect.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace krkrsdl3 { namespace update_region {
// Window completion walks the whole Layer tree once per update rectangle.
// On a GPU every rectangle pays a fixed chain of render passes (DrawBuffer,
// temporaries, transition outputs), which device logs fit at ~51us per pass
// versus ~0.57ms per logical Mpx. ComplexRect banding instead splits one text
// line into 13x1 / 39x1 slivers that each pay the whole chain.
//
// Redrawing extra pixels of the current Layer tree is pixel-exact (it is what
// TVP_UPDATE_UNITE_LIMIT already does), so merge rectangles into disjoint
// bounding boxes whenever the added area costs less than the passes saved.
// One rectangle's chain is valued as PassPixels of extra composition.
constexpr uint64_t DefaultPassPixels = 65536;
// Greedy merging is quadratic in boxes; beyond this only the full bounding
// box is considered, which keeps worst-case completion cost bounded.
constexpr size_t GreedyLimit = 32;

struct Result { size_t rectanglesBefore = 0, rectanglesAfter = 0; uint64_t pixelsBefore = 0, pixelsAfter = 0; };

inline uint64_t Area(const tTVPRect& r) noexcept {
    return r.is_empty() ? 0 : uint64_t(r.get_width()) * uint64_t(r.get_height());
}
inline uint64_t Cost(size_t count, uint64_t pixels, uint64_t passPixels) noexcept {
    return uint64_t(count) * passPixels + pixels;
}

// Returns the disjoint boxes that cover "input"; inputs must be disjoint.
inline std::vector<tTVPRect> Merge(std::vector<tTVPRect> boxes, uint64_t passPixels) {
    if (boxes.size() < 2) return boxes;
    if (boxes.size() > GreedyLimit) {
        tTVPRect bound = boxes[0]; uint64_t pixels = 0;
        for (const auto& b : boxes) { bound.do_union(b); pixels += Area(b); }
        if (Cost(1, Area(bound), passPixels) < Cost(boxes.size(), pixels, passPixels)) return {bound};
        return boxes;
    }
    std::vector<char> members(boxes.size());
    while (true) {
        size_t bestA = 0, bestB = 0; uint64_t bestGain = 0; tTVPRect bestBox;
        for (size_t a = 0; a < boxes.size(); ++a) for (size_t b = a + 1; b < boxes.size(); ++b) {
            // The merged box must not overlap any other box; absorb those too.
            tTVPRect box = boxes[a]; box.do_union(boxes[b]);
            std::fill(members.begin(), members.end(), 0); members[a] = members[b] = 1;
            for (bool grew = true; grew;) {
                grew = false;
                for (size_t c = 0; c < boxes.size(); ++c)
                    if (!members[c] && box.intersects_with_no_empty_check(boxes[c])) {
                        members[c] = 1; box.do_union(boxes[c]); grew = true;
                    }
            }
            size_t count = 0; uint64_t pixels = 0;
            for (size_t c = 0; c < boxes.size(); ++c) if (members[c]) { ++count; pixels += Area(boxes[c]); }
            const uint64_t before = Cost(count, pixels, passPixels), after = Cost(1, Area(box), passPixels);
            if (after < before && before - after > bestGain) {
                bestGain = before - after; bestA = a; bestB = b; bestBox = box;
            }
        }
        if (!bestGain) return boxes;
        // Recompute the winning closure; keep every box it does not absorb.
        std::vector<tTVPRect> next;
        next.reserve(boxes.size());
        next.push_back(bestBox);
        for (size_t c = 0; c < boxes.size(); ++c)
            if (c != bestA && c != bestB && !bestBox.intersects_with_no_empty_check(boxes[c])) next.push_back(boxes[c]);
        boxes.swap(next);
    }
}

// Rewrites "region" only when merging lowers the modeled composition cost.
inline Result Coalesce(tTVPComplexRect& region, uint64_t passPixels = DefaultPassPixels) {
    Result result;
    std::vector<tTVPRect> boxes;
    boxes.reserve(size_t(region.GetCount()));
    auto it = region.GetIterator();
    while (it.Step()) { boxes.push_back(*it); result.pixelsBefore += Area(*it); }
    result.rectanglesBefore = result.rectanglesAfter = boxes.size();
    result.pixelsAfter = result.pixelsBefore;
    if (boxes.size() < 2 || !passPixels) return result;
    const auto merged = Merge(boxes, passPixels);
    if (merged.size() == boxes.size()) return result;
    region.Clear();
    result.pixelsAfter = 0;
    for (const auto& box : merged) { region.Or(box); result.pixelsAfter += Area(box); }
    result.rectanglesAfter = size_t(region.GetCount());
    return result;
}
} }
