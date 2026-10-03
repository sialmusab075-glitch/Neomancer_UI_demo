#pragma once

#include "neo/dsa/HashMap.h"
#include "neo/dsa/Instrumentation.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace neo {
namespace dsa {

// IndexedHashMap: the stage 7 memory experiment. The same Robin Hood table as
// HashMap, but a slot holds only a record INDEX; the key is never stored. A
// lookup finds the key by asking the master vector (keyOf(record)).
//
//   HashMap<std::string, uint32_t>   slot = key (32 B) + value (4 B) + distance (4 B), padded
//   IndexedHashMap                   slot = record (4 B) + meta (4 B) = 8 B
//
// meta packs the probe distance (upper 24 bits) with an 8-bit tag taken from the
// top of the hash (lower 8 bits). The tag is what keeps this fast: a probe only
// dereferences the master vector (a likely cache miss) when the tag matches, so a
// failed probe costs about 1/256 of an indirection instead of one.
//
// WHAT IT COSTS
//   * every key comparison that gets past the tag is one extra memory access;
//   * growth recomputes each hash from keyOf(record), so rehash reads every key;
//   * the map is only valid while the master vector it points into is unchanged
//     and the keys it was built from do not change: insert(record) stores a
//     position, so the caller owns keeping records and index in step.
//
// COMPLEXITY is that of HashMap: O(1) expected find / insert / erase, load < 0.75.
//
// KeyOf: callable `const Key& (std::uint32_t record)` (or by value); it must return
// the same key for the same record for as long as the record is in the map.
template <class KeyOf,
          class Key = std::decay_t<std::invoke_result_t<const KeyOf&, std::uint32_t>>,
          class Hash = DefaultHash<Key>, class Counters = NullCounters>
class IndexedHashMap : private Counters {
public:
    static constexpr std::uint32_t kNotFound = 0xFFFFFFFFu;
    static constexpr std::size_t   kMinCapacity = 8;

    explicit IndexedHashMap(KeyOf keyOf, std::size_t expectedEntries = 0, Hash hash = Hash())
        : keyOf_(std::move(keyOf)), hash_(std::move(hash)) {
        if (expectedEntries > 0) {
            reserve(expectedEntries);
        }
    }

    // Inserts record, or replaces the record already stored under the same key.
    // Returns true when the key was new.
    bool insert(std::uint32_t record) {
        if (slots_.empty() || (size_ + 1) * 4 > capacity() * 3) { // load factor 0.75
            growTo(slots_.empty() ? kMinCapacity : capacity() * 2);
        }
        return insertNoGrow(record, hash64(keyOf_(record)));
    }

    // The record stored under `key`, or kNotFound.
    std::uint32_t find(const Key& key) const {
        const std::size_t slot = locate(key);
        return slot == npos ? kNotFound : slots_[slot].record;
    }
    bool contains(const Key& key) const { return locate(key) != npos; }

    // Backward-shift deletion, exactly as HashMap does it.
    bool erase(const Key& key) {
        const std::size_t slot = locate(key);
        if (slot == npos) {
            return false;
        }
        std::size_t hole = slot;
        for (;;) {
            const std::size_t next = (hole + 1) & mask_;
            Slot& following = slots_[next];
            if (following.meta == kFreeMeta || distanceOf(following.meta) == 0) {
                break;
            }
            slots_[hole] = following;
            slots_[hole].meta = withDistance(slots_[hole].meta, distanceOf(slots_[hole].meta) - 1);
            this->moveOp();
            hole = next;
        }
        slots_[hole] = Slot();
        --size_;
        return true;
    }

    void clear() {
        for (Slot& slot : slots_) {
            slot = Slot();
        }
        size_ = 0;
    }

    void reserve(std::size_t entries) {
        std::size_t wanted = kMinCapacity;
        while (entries * 4 > wanted * 3) {
            wanted *= 2;
        }
        if (wanted > capacity()) {
            growTo(wanted);
        }
    }

    std::size_t size() const { return size_; }
    bool        empty() const { return size_ == 0; }
    std::size_t capacity() const { return slots_.size(); }
    std::size_t memoryBytes() const { return slots_.capacity() * sizeof(Slot); }
    double loadFactor() const {
        return slots_.empty() ? 0.0 : static_cast<double>(size_) / static_cast<double>(slots_.size());
    }

    ProbeStats probeStats() const {
        ProbeStats stats;
        stats.size = size_;
        stats.capacity = capacity();
        stats.loadFactor = loadFactor();
        std::uint64_t total = 0;
        for (const Slot& slot : slots_) {
            if (slot.meta == kFreeMeta) {
                continue;
            }
            const std::size_t d = distanceOf(slot.meta);
            total += d;
            stats.maxProbe = d > stats.maxProbe ? d : stats.maxProbe;
            if (stats.histogram.size() <= d) {
                stats.histogram.resize(d + 1, 0);
            }
            ++stats.histogram[d];
        }
        stats.meanProbe = size_ == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(size_);
        return stats;
    }

    // Same promises as HashMap::checkInvariants, plus: the stored tag is the tag of
    // the key the record really has.
    bool checkInvariants(std::string& error) const {
        std::size_t counted = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const Slot& slot = slots_[i];
            if (slot.meta == kFreeMeta) {
                continue;
            }
            ++counted;
            const std::uint64_t h = hash64(keyOf_(slot.record));
            const std::size_t ideal = static_cast<std::size_t>(h) & mask_;
            const std::size_t expected = (i + slots_.size() - ideal) & mask_;
            if (expected != distanceOf(slot.meta)) {
                error = "slot " + std::to_string(i) + " stores distance " + std::to_string(distanceOf(slot.meta)) +
                        " but sits " + std::to_string(expected) + " from its ideal slot";
                return false;
            }
            if (tagOf(slot.meta) != tagFor(h)) {
                error = "slot " + std::to_string(i) + " stores a tag that is not its key's tag";
                return false;
            }
            for (std::size_t step = 0; step < expected; ++step) {
                if (slots_[(ideal + step) & mask_].meta == kFreeMeta) {
                    error = "a free slot sits between the ideal and actual position of an entry";
                    return false;
                }
            }
        }
        if (counted != size_) {
            error = "size() says " + std::to_string(size_) + " but " + std::to_string(counted) + " slots are used";
            return false;
        }
        return true;
    }

    const Counters& counters() const { return *this; }
    Counters& counters() { return *this; }

private:
    static constexpr std::size_t   npos = static_cast<std::size_t>(-1);
    static constexpr std::uint32_t kFreeMeta = 0xFFFFFFFFu;

    struct Slot {
        std::uint32_t record = 0;
        std::uint32_t meta = kFreeMeta; // distance << 8 | tag; all ones = free
    };

    static std::uint32_t distanceOf(std::uint32_t meta) { return meta >> 8; }
    static std::uint32_t tagOf(std::uint32_t meta) { return meta & 0xFFu; }
    static std::uint32_t withDistance(std::uint32_t meta, std::uint32_t d) { return (d << 8) | (meta & 0xFFu); }
    static std::uint32_t tagFor(std::uint64_t h) { return static_cast<std::uint32_t>(h >> 56); }

    std::uint64_t hash64(const Key& key) const { return static_cast<std::uint64_t>(hash_(key)); }

    std::size_t locate(const Key& key) const {
        if (slots_.empty()) {
            return npos;
        }
        const std::uint64_t h = hash64(key);
        const std::uint32_t tag = tagFor(h);
        std::size_t index = static_cast<std::size_t>(h) & mask_;
        std::uint32_t distance = 0;
        for (;;) {
            const Slot& slot = slots_[index];
            this->probe();
            if (slot.meta == kFreeMeta) {
                return npos;
            }
            if (distanceOf(slot.meta) < distance) {
                return npos; // Robin Hood ordering: the key cannot be further on
            }
            if (tagOf(slot.meta) == tag && keyOf_(slot.record) == key) {
                return index;
            }
            index = (index + 1) & mask_;
            ++distance;
        }
    }

    bool insertNoGrow(std::uint32_t record, std::uint64_t h) {
        std::size_t index = static_cast<std::size_t>(h) & mask_;
        std::uint32_t carriedRecord = record;
        std::uint32_t carriedTag = tagFor(h);
        std::uint32_t carriedDistance = 0;

        for (;;) {
            Slot& slot = slots_[index];
            this->probe();
            if (slot.meta == kFreeMeta) {
                slot.record = carriedRecord;
                slot.meta = (carriedDistance << 8) | carriedTag;
                ++size_;
                return true;
            }
            if (distanceOf(slot.meta) == carriedDistance && tagOf(slot.meta) == carriedTag &&
                keyOf_(slot.record) == keyOf_(carriedRecord)) {
                slot.record = carriedRecord; // same key: replace, size unchanged
                return false;
            }
            if (distanceOf(slot.meta) < carriedDistance) {
                const std::uint32_t residentRecord = slot.record;
                const std::uint32_t residentMeta = slot.meta;
                slot.record = carriedRecord;
                slot.meta = (carriedDistance << 8) | carriedTag;
                carriedRecord = residentRecord;
                carriedTag = tagOf(residentMeta);
                carriedDistance = distanceOf(residentMeta);
                this->swapOp();
            }
            index = (index + 1) & mask_;
            ++carriedDistance;
        }
    }

    void growTo(std::size_t newCapacity) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(newCapacity, Slot());
        mask_ = newCapacity - 1;
        size_ = 0;
        for (const Slot& slot : old) {
            if (slot.meta != kFreeMeta) {
                insertNoGrow(slot.record, hash64(keyOf_(slot.record)));
            }
        }
    }

    KeyOf             keyOf_;
    Hash              hash_;
    std::vector<Slot> slots_;
    std::size_t       size_ = 0;
    std::size_t       mask_ = 0;
};

} // namespace dsa
} // namespace neo
