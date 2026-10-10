#pragma once
#include "LayerRenderOperation.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace krkrsdl3 {
enum class PresentationFailure : unsigned {
    None, Unsupported, NoDrawable, NotDisplayed, EncodingFailed, CommandFailed
};
enum class PresentationCommand : unsigned { Pending, Succeeded, Failed };
inline const char* PresentationFailureName(PresentationFailure reason) noexcept {
    switch(reason) {
        case PresentationFailure::None:return "none";
        case PresentationFailure::Unsupported:return "unsupported";
        case PresentationFailure::NoDrawable:return "noDrawable";
        case PresentationFailure::NotDisplayed:return "notDisplayed";
        case PresentationFailure::EncodingFailed:return "encodingFailed";
        case PresentationFailure::CommandFailed:return "commandFailed";
    }
    return "unknown";
}
// One ticket per display frame, shared by reads encoded before its presentation.
// GPU completion and actual presentation are deliberately separate conditions.
struct AsyncLayerPresentation {
    explicit AsyncLayerPresentation(uint64_t serial) : frameSerial(serial) {}
    const uint64_t frameSerial;
    std::atomic<bool> presented{false}, failed{false};
    std::atomic<double> presentedTime{0};
    // C++ callback metadata only; does not change the Swift/C profile ABI.
    // A later GPU failure must dominate an earlier unavailable display surface.
    std::atomic<PresentationFailure> failureReason{PresentationFailure::None};
    std::atomic<PresentationCommand> command{PresentationCommand::Pending};
    void MarkFailed(PresentationFailure reason) noexcept {
        auto previous=failureReason.load(std::memory_order_relaxed);
        while(previous<reason && !failureReason.compare_exchange_weak(previous,reason,
              std::memory_order_release,std::memory_order_relaxed)) {}
        failed.store(true,std::memory_order_release);
    }
    void MarkCommandCompleted(bool success) noexcept {
        if(success) {
            auto pending=PresentationCommand::Pending;
            command.compare_exchange_strong(pending,PresentationCommand::Succeeded,
                std::memory_order_release,std::memory_order_relaxed);
        } else {
            MarkFailed(PresentationFailure::CommandFailed);
            command.store(PresentationCommand::Failed,std::memory_order_release);
        }
    }
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
