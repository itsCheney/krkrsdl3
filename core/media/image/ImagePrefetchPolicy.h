#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace krkrsdl3::image_prefetch {
// A cancelled ticket occupies its slot until drained. Repeated session resets
// cannot accumulate unbounded cancelled queue entries behind a running decode.
inline std::atomic<uint64_t> generation{1};
inline void Cancel() noexcept {generation.fetch_add(1,std::memory_order_relaxed);}
struct Budget { uint64_t remaining,deadline;Budget(uint64_t bytes,uint64_t until):remaining(bytes),deadline(until) {} };
struct Ticket { uint64_t id=0,epoch=0;std::shared_ptr<Budget> budget; };
template<class Key> class Policy {
    struct Slot {Key key;uint64_t id=0,epoch=0;};
    std::array<Slot,32> slots{};
    std::mutex mutex;
    uint64_t nextID=0;
public:
    bool CanAdmit(const std::shared_ptr<Budget>& budget,uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!budget || !budget->remaining || (budget->deadline && now>=budget->deadline))return false;
        for(const auto& slot:slots)if(!slot.id)return true;
        return false;
    }
    Ticket Enqueue(const Key& key,const std::shared_ptr<Budget>& budget,uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto epoch=generation.load(std::memory_order_relaxed);
        if(!budget || !budget->remaining || (budget->deadline && now>=budget->deadline)) return {};
        Slot* available=nullptr;
        for(auto& slot:slots) {
            if(slot.id && slot.epoch==epoch && slot.key==key) return {};
            if(!slot.id && !available) available=&slot;
        }
        if(!available) return {};
        available->key=key;available->epoch=epoch;available->id=++nextID;
        return {available->id,epoch,budget};
    }
    bool Current(const Ticket& ticket,uint64_t now) noexcept {
        return ticket.id && ticket.epoch==generation.load(std::memory_order_relaxed) && ticket.budget &&
            (!ticket.budget->deadline || now<ticket.budget->deadline);
    }
    bool Reserve(const Ticket& ticket,uint64_t bytes,uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!Current(ticket,now) || bytes>ticket.budget->remaining) return false;
        ticket.budget->remaining-=bytes;return true;
    }
    void Finish(const Ticket& ticket) {
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& slot:slots) if(slot.id==ticket.id && slot.epoch==ticket.epoch) {slot.id=0;slot.key=Key{};return;}
    }
};
}
