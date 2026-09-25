#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace emoteplayer
{

// Runtime integration owns the concrete decoded-resource cache and implements
// these hooks for session shutdown and canonical-path invalidation.
struct EmoteSharedResourceCacheStats
{
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::size_t retainedBytes = 0;
    std::size_t entries = 0;
    std::uint64_t generation = 0;
};

void ClearSharedEmoteResourceCache();
void InvalidateSharedEmoteResource(const std::string& canonicalPath);
EmoteSharedResourceCacheStats GetSharedEmoteResourceCacheStats();

// A retention budget for immutable, fully constructed CPU resource data. Value
// must not own script VM or GPU objects, and callers must not retain mutable
// aliases to a published value. The budget covers caller-supplied resource cost;
// handles held by players can outlive eviction and are not part of retainedBytes.
template <typename Key, typename Value, typename Compare = std::less<Key>>
class EmoteResourceCache
{
public:
    using Handle = std::shared_ptr<const Value>;

    struct Stats
    {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::size_t retainedBytes = 0;
        std::size_t entries = 0;
        std::uint64_t generation = 0;
    };

    explicit EmoteResourceCache(std::size_t budgetBytes) : _budgetBytes(budgetBytes) {}
    EmoteResourceCache(const EmoteResourceCache&) = delete;
    EmoteResourceCache& operator=(const EmoteResourceCache&) = delete;

    Handle Find(const Key& key)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _index.find(key);
        if (found == _index.end()) {
            ++_misses;
            return {};
        }
        ++_hits;
        _order.splice(_order.begin(), _order, found->second);
        return found->second->value;
    }

    // Construct/decode the value before calling Insert: failed work is never
    // published. Zero-cost, null and oversized resources leave the cache intact.
    // A generation captured before decoding prevents an in-flight load from
    // repopulating a cache cleared by session shutdown or decoder invalidation.
    bool Insert(const Key& key, Handle value, std::size_t costBytes,
                std::optional<std::uint64_t> expectedGeneration = std::nullopt)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!value || costBytes == 0 || costBytes > _budgetBytes ||
            (expectedGeneration && *expectedGeneration != _generation))
            return false;

        auto found = _index.find(key);
        if (found != _index.end()) {
            auto entry = found->second;
            _retainedBytes -= entry->cost;
            entry->value = std::move(value);
            entry->cost = costBytes;
            _order.splice(_order.begin(), _order, entry);
        } else {
            // Allocate both index and LRU node before evicting existing data.
            // If either allocation/key copy fails, all retained data survives.
            _order.push_front(Entry{std::move(value), costBytes, {}});
            try {
                const auto inserted = _index.emplace(key, _order.begin());
                if (!inserted.second) {
                    _order.pop_front();
                    return false;
                }
                _order.front().indexed = inserted.first;
            } catch (...) {
                _order.pop_front();
                throw;
            }
        }

        // The front node has not been charged yet. Subtracting its cost from
        // the budget avoids overflow even when a caller uses SIZE_MAX capacity.
        while (_retainedBytes > _budgetBytes - costBytes) {
            const auto& victim = _order.back();
            // Erase by iterator so eviction neither copies nor compares keys.
            _index.erase(victim.indexed);
            _retainedBytes -= victim.cost;
            _order.pop_back();
            ++_evictions;
        }
        _retainedBytes += costBytes;
        return true;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _index.clear();
        _order.clear();
        _retainedBytes = 0;
        ++_generation;
        // Counters are cumulative across invalidations; a clear is not an LRU
        // eviction. This keeps diagnostics useful when managers come and go.
    }

    template <typename Predicate>
    std::size_t EraseIf(Predicate predicate)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // Increment even when no key matches: a matching decode may still be
        // in flight and must not publish against the old generation.
        ++_generation;
        std::size_t removed = 0;
        for (auto entry = _index.begin(); entry != _index.end();) {
            if (!predicate(entry->first)) {
                ++entry;
                continue;
            }
            _retainedBytes -= entry->second->cost;
            _order.erase(entry->second);
            entry = _index.erase(entry);
            ++removed;
        }
        return removed;
    }

    std::uint64_t Generation() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _generation;
    }

    Stats GetStats() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return {_hits, _misses, _evictions, _retainedBytes, _index.size(), _generation};
    }

    std::size_t BudgetBytes() const { return _budgetBytes; }

private:
    struct Entry;
    using Order = std::list<Entry>;
    using Index = std::map<Key, typename Order::iterator, Compare>;

    struct Entry
    {
        Handle value;
        std::size_t cost;
        typename Index::iterator indexed;
    };

    const std::size_t _budgetBytes;
    mutable std::mutex _mutex;
    Order _order;
    Index _index;
    std::size_t _retainedBytes = 0;
    std::uint64_t _hits = 0;
    std::uint64_t _misses = 0;
    std::uint64_t _evictions = 0;
    std::uint64_t _generation = 0;
};

} // namespace emoteplayer
