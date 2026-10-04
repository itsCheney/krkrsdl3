#pragma once

// Canvas validity is independent of diagnostics and of the animation clock.
// This small production state machine is also compiled directly by the portable tests.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace emoteplayer::performance {

inline std::uint64_t nextPlayerIdentity() {
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}
struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
    std::uint64_t pixels() const {
        return empty() ? 0 : std::uint64_t(right - left) * std::uint64_t(bottom - top);
    }
};
inline Rect unite(Rect a, Rect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.left,b.left),std::min(a.top,b.top),
            std::max(a.right,b.right),std::max(a.bottom,b.bottom)};
}
inline Rect clip(Rect r, int width, int height) {
    return {std::clamp(r.left,0,width),std::clamp(r.top,0,height),
            std::clamp(r.right,0,width),std::clamp(r.bottom,0,height)};
}
struct Bounds { bool known = true; Rect rect; };
struct TextureKey {
    std::uint64_t identity = 0, version = 0;
    bool valid = false;
    bool operator==(const TextureKey& other) const {
        return valid && other.valid && identity == other.identity && version == other.version;
    }
};
struct DrawToken {
    std::uint64_t player = 0, pose = 0;
    bool operator==(const DrawToken& other) const {
        return player == other.player && pose == other.pose;
    }
};
struct CaptureDecision {
    bool skip = false, canRegionCopy = false;
    Rect update;
    std::uint64_t sourceVersion = 0;
};

class CanvasCaptureCache {
    static constexpr std::size_t maxRecipe = 256, maxDestinations = 16;
    struct Destination {
        std::uintptr_t layer = 0;
        TextureKey texture;
        std::uint64_t sourceVersion = 0, used = 0;
        Bounds bounds;
    };
    int width_ = 0, height_ = 0;
    bool clearRecipe_ = false, recipeOverflow_ = false, pending_ = true;
    std::vector<DrawToken> recipe_, resolvedRecipe_;
    Bounds bounds_{false,{}};
    bool resolvedClear_ = false, resolvedOverflow_ = true;
    std::uint64_t sourceVersion_ = 0, clock_ = 0;
    std::vector<Destination> destinations_;
public:
    void reset(int width, int height) {
        width_ = width; height_ = height;
        destinations_.clear(); recipe_.clear(); resolvedRecipe_.clear();
        resolvedOverflow_ = true; sourceVersion_ = 0;
        beginClear(); // newly created render targets are transparent.
    }
    void beginClear() {
        recipe_.clear(); clearRecipe_ = true; recipeOverflow_ = false;
        bounds_ = {}; pending_ = true;
    }
    void invalidate() {
        // Unknown source writers cannot produce a reproducible clear+draw recipe.
        clearRecipe_ = false; recipeOverflow_ = true; bounds_.known = false;
        pending_ = true;
    }
    void recordDraw(DrawToken token, Bounds bounds) {
        if (recipe_.size() < maxRecipe) recipe_.push_back(token);
        else recipeOverflow_ = true;
        bounds_.known = bounds_.known && bounds.known;
        bounds_.rect = unite(bounds_.rect,bounds.rect);
        pending_ = true;
    }
    // Only a private target with a self-cleared, single-player recipe may omit
    // the next clear/draw. Shared/accumulating canvases must encode every draw.
    bool canRetainSingleDraw(DrawToken token) const {
        return clearRecipe_ && !recipeOverflow_ && recipe_.size() == 1 && recipe_[0] == token;
    }
    std::uint64_t resolveSourceVersion() {
        if (!pending_) return sourceVersion_;
        // Equal, complete, ordered recipes have the same canvas contents, even
        // when scripts clear and submit the same drawing commands again.
        if (sourceVersion_ == 0 || recipeOverflow_ || resolvedOverflow_ ||
            !clearRecipe_ || !resolvedClear_ || recipe_ != resolvedRecipe_) {
            ++sourceVersion_;
        }
        resolvedRecipe_ = recipe_; resolvedClear_ = clearRecipe_;
        resolvedOverflow_ = recipeOverflow_; pending_ = false;
        return sourceVersion_;
    }
    CaptureDecision decide(std::uintptr_t layer, TextureKey texture) {
        CaptureDecision out;
        out.sourceVersion = resolveSourceVersion();
        out.update = {0,0,width_,height_};
        auto found = std::find_if(destinations_.begin(),destinations_.end(),
            [layer](const Destination& d) { return d.layer == layer; });
        if (found == destinations_.end() || !(found->texture == texture)) return out;
        found->used = ++clock_;
        if (found->sourceVersion == out.sourceVersion) { out.skip = true; return out; }
        if (clearRecipe_ && bounds_.known && found->bounds.known) {
            out.update = clip(unite(bounds_.rect,found->bounds.rect),width_,height_);
            out.canRegionCopy = true;
        }
        return out;
    }
    void commit(std::uintptr_t layer, TextureKey texture, std::uint64_t version) {
        if (!texture.valid) return; // unobservable external writes disable caching.
        auto found = std::find_if(destinations_.begin(),destinations_.end(),
            [layer](const Destination& d) { return d.layer == layer; });
        if (found == destinations_.end()) {
            if (destinations_.size() == maxDestinations)
                found = std::min_element(destinations_.begin(),destinations_.end(),
                    [](const Destination& a,const Destination& b) { return a.used < b.used; });
            else { destinations_.push_back({}); found = destinations_.end()-1; }
        }
        *found = {layer,texture,version,++clock_,bounds_};
    }
    Bounds bounds() const { return bounds_; }
};
} // namespace emoteplayer::performance
