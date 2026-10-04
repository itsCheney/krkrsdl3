#pragma once
#include "LayerRenderOperation.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace krkrsdl3 {
// One ticket per display frame, shared by reads encoded before its presentation.
// GPU completion and actual presentation are deliberately separate conditions.
struct AsyncLayerPresentation {
    explicit AsyncLayerPresentation(uint64_t serial) : frameSerial(serial) {}
    const uint64_t frameSerial;
    std::atomic<bool> presented{false}, failed{false};
    std::atomic<double> presentedTime{0};
};
struct AsyncLayerReadback {
    TVPLayerRect region{};
    std::shared_ptr<AsyncLayerPresentation> presentation;
    std::shared_ptr<void> allocationLease;
    std::vector<uint8_t> rgba;
    int pitch = 0;
    std::atomic<bool> completed{false}, failed{false}, canceled{false};
    // Backend writes rgba/pitch once, then publishes completed with release.
    // Consumers acquire completed before observing those immutable values.
    bool Ready() const {
        return completed.load(std::memory_order_acquire) &&
            !failed.load(std::memory_order_acquire) && !canceled.load(std::memory_order_acquire) &&
            presentation && presentation->presented.load(std::memory_order_acquire) &&
            !presentation->failed.load(std::memory_order_acquire);
    }
    // One render-thread consumer, after acquiring completed. Metal may retain
    // the completion block/request longer than the consumer needs its storage.
    void ReleaseStorage() {
        if (!completed.load(std::memory_order_acquire)) return;
        std::vector<uint8_t>().swap(rgba);
        allocationLease.reset();
    }
};
}
