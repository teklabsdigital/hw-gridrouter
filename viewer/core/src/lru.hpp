// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// lru.hpp: a small least-recently-used cache of immutable values.
//
// Bounded by both an entry count and a byte budget.  The most recently used
// entry is always kept, even if it alone exceeds the byte budget, so a
// single huge wavefront can still be shown.  Values are shared_ptr<const V>:
// a caller that holds one keeps it alive after eviction.
#ifndef RV_LRU_HPP
#define RV_LRU_HPP

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <utility>

namespace rv {

struct CacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

template <class V>
class Lru {
public:
    Lru(std::size_t max_entries, std::size_t max_bytes)
        : max_entries_(max_entries < 1 ? 1 : max_entries), max_bytes_(max_bytes) {}

    void set_limits(std::size_t max_entries, std::size_t max_bytes) {
        max_entries_ = max_entries < 1 ? 1 : max_entries;
        max_bytes_ = max_bytes;
        trim();
    }

    // The cached value for key, moved to the front; nullptr on a miss.
    std::shared_ptr<const V> get(std::uint32_t key) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->key == key) {
                items_.splice(items_.begin(), items_, it);
                ++stats_.hits;
                return items_.front().value;
            }
        }
        ++stats_.misses;
        return nullptr;
    }

    // Insert a value as the most recently used, then evict from the back.
    void put(std::uint32_t key, std::shared_ptr<const V> value, std::size_t bytes) {
        items_.push_front(Item{key, std::move(value), bytes});
        stats_.bytes += bytes;
        ++stats_.entries;
        trim();
    }

    // Keys from most to least recently used (for tests).
    template <class F>
    void for_each_key(F f) const {
        for (const Item &item : items_) f(item.key);
    }

    void clear() {
        items_.clear();
        stats_.entries = 0;
        stats_.bytes = 0;
    }

    const CacheStats &stats() const { return stats_; }

private:
    struct Item {
        std::uint32_t key;
        std::shared_ptr<const V> value;
        std::size_t bytes;
    };

    void trim() {
        while (items_.size() > 1 &&
               (items_.size() > max_entries_ || stats_.bytes > max_bytes_)) {
            stats_.bytes -= items_.back().bytes;
            --stats_.entries;
            ++stats_.evictions;
            items_.pop_back();
        }
    }

    std::size_t max_entries_;
    std::size_t max_bytes_;
    std::list<Item> items_;
    CacheStats stats_;
};

} // namespace rv

#endif // RV_LRU_HPP
