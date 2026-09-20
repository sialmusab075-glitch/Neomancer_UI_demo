#pragma once

#include "neo/dsa/Instrumentation.h"
#include "neo/model/Hash.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neo {
namespace dsa {

// HashMap: open addressing with Robin Hood probing and backward-shift deletion.
//
// WHAT IT IS FOR
//   The exact-lookup index of the project: primary designation -> record index
//   ("433" -> 17), and SPK-ID -> record index as a secondary key. Every value
//   stored is a std::uint32_t position into the master vector, never a record.
//
// COMPLEXITY (n entries, load factor a = n / capacity, kept below 0.75)
//   find / insert / erase   O(1) expected, O(n) worst case
//   iteration (forEach)     O(capacity)
//   rehash on growth        O(n), amortised O(1) per insert
//   space                   capacity * sizeof(Slot), capacity a power of two,
//                           so n / 0.75 <= capacity < 2n / 0.75
//
// WHY THIS DESIGN
//   * Open addressing over separate chaining: every probe walks contiguous
//     memory instead of following a pointer per node, and there is one
//     allocation for the whole table rather than one per entry. With 42k
//     designations that is the difference between one 2 MB block and 42k tiny
//     ones.
//   * Robin Hood probing: on insert, an entry that has travelled further from
//     its ideal slot displaces one that has travelled less ("steal from the
//     rich"). The mean probe length is unchanged, but the variance collapses:
//     the worst lookup in the table stays close to the average, which is what
//     makes the structure predictable rather than merely fast on average.
//   * Backward-shift deletion instead of tombstones: erasing shifts the
//     following run one slot back, so the table never fills with dead markers
//     that slow every later probe. The cost is paid once at erase time, and a
//     delete-heavy workload does not degrade.
//   * Power-of-two capacity: the modulo becomes a mask (hash & mask).
//   * Growth at 0.75: past that, probe lengths in an open-addressed table climb
//     sharply (the expected probe count for linear probing grows like
//     1/(1-a)^2).
//
// The Counters policy is inherited privately: with NullCounters it is an empty
// base and costs nothing.

// Default hashing. Strings go through the same FNV-1a the response cache uses;
// integers are passed through a mixer, because the low bits of an SPK-ID or a
// record index are not well distributed on their own and a masked table would
// see long runs of collisions.
template <class Key>
struct DefaultHash;

template <>
struct DefaultHash<std::string> {
    std::uint64_t operator()(const std::string& key) const { return fnv1a64(key); }
};

template <>
struct DefaultHash<std::uint32_t> {
    std::uint64_t operator()(std::uint32_t key) const {
        // splitmix64 finaliser: cheap, and spreads low bits across the word.
        std::uint64_t x = static_cast<std::uint64_t>(key) + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
};

template <>
struct DefaultHash<std::uint64_t> {
    std::uint64_t operator()(std::uint64_t key) const {
        std::uint64_t x = key + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
};

struct ProbeStats {
    std::size_t size = 0;
    std::size_t capacity = 0;
    double      loadFactor = 0.0;
    double      meanProbe = 0.0;  // mean distance from the ideal slot, in probes
    std::size_t maxProbe = 0;
    // histogram[d] = number of entries sitting d slots from their ideal one.
    std::vector<std::size_t> histogram;
};

template <class Key, class Value, class Hash = DefaultHash<Key>, class Counters = NullCounters>
class HashMap : private Counters {
public:
    static constexpr std::uint32_t kFree = 0xFFFFFFFFu; // "slot is empty"
    static constexpr std::size_t   kMinCapacity = 8;

    HashMap() = default;
    explicit HashMap(std::size_t expectedEntries) { reserve(expectedEntries); }

    // Inserts or overwrites. Returns true when the key was new.
    bool insert(const Key& key, const Value& value) {
        if (slots_.empty() || (size_ + 1) * 4 > capacity() * 3) { // load factor 0.75
            growTo(slots_.empty() ? kMinCapacity : capacity() * 2);
        }
        return insertNoGrow(key, value);
    }

    Value* find(const Key& key) {
        const std::size_t slot = locate(key);
        return slot == npos ? nullptr : &slots_[slot].value;
    }
    const Value* find(const Key& key) const {
        const std::size_t slot = locate(key);
        return slot == npos ? nullptr : &slots_[slot].value;
    }
    bool contains(const Key& key) const { return locate(key) != npos; }

    // Removes a key. Returns false when it was not present.
    // The run after the hole is shifted back one slot at a time, stopping at
    // the first entry that is already in its ideal slot (distance 0) or at a
    // free slot: exactly the entries that could have probed past the hole.
    bool erase(const Key& key) {
        const std::size_t slot = locate(key);
        if (slot == npos) {
            return false;
        }
        std::size_t hole = slot;
        for (;;) {
            const std::size_t next = (hole + 1) & mask_;
            Slot& following = slots_[next];
            if (following.distance == kFree || following.distance == 0) {
                break;
            }
            slots_[hole] = std::move(following);
            slots_[hole].distance -= 1;
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

    // Sizes the table so `entries` fit below the load factor without rehashing.
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
    double loadFactor() const {
        return slots_.empty() ? 0.0 : static_cast<double>(size_) / static_cast<double>(slots_.size());
    }

    // fn(const Key&, const Value&) for every entry, in slot order.
    template <class Fn>
    void forEach(Fn&& fn) const {
        for (const Slot& slot : slots_) {
            if (slot.distance != kFree) {
                fn(slot.key, slot.value);
            }
        }
    }

    ProbeStats probeStats() const {
        ProbeStats stats;
        stats.size = size_;
        stats.capacity = capacity();
        stats.loadFactor = loadFactor();
        std::uint64_t total = 0;
        for (const Slot& slot : slots_) {
            if (slot.distance == kFree) {
                continue;
            }
            const std::size_t d = slot.distance;
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

    // Checks the structural promises, for the fuzz test:
    //   * every entry's stored distance matches where its hash says it belongs
    //   * no free slot sits between an entry's ideal slot and its actual one
    //     (that would make a lookup give up too early)
    //   * the entry count matches size()
    bool checkInvariants(std::string& error) const {
        std::size_t counted = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const Slot& slot = slots_[i];
            if (slot.distance == kFree) {
                continue;
            }
            ++counted;
            const std::size_t ideal = static_cast<std::size_t>(hash_(slot.key)) & mask_;
            const std::size_t expected = (i + slots_.size() - ideal) & mask_;
            if (expected != slot.distance) {
                error = "slot " + std::to_string(i) + " stores distance " + std::to_string(slot.distance) +
                        " but sits " + std::to_string(expected) + " from its ideal slot";
                return false;
            }
            for (std::size_t step = 0; step < slot.distance; ++step) {
                if (slots_[(ideal + step) & mask_].distance == kFree) {
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
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    struct Slot {
        Key           key = Key();
        Value         value = Value();
        std::uint32_t distance = kFree; // slots travelled from the ideal one
    };

    std::size_t locate(const Key& key) const {
        if (slots_.empty()) {
            return npos;
        }
        std::size_t index = static_cast<std::size_t>(hash_(key)) & mask_;
        std::uint32_t distance = 0;
        for (;;) {
            const Slot& slot = slots_[index];
            this->probe();
            if (slot.distance == kFree) {
                return npos; // a free slot ends the run: the key cannot be further on
            }
            if (slot.distance < distance) {
                // Robin Hood ordering: anything beyond here is closer to its
                // ideal slot than we would be, so our key is not in the table.
                return npos;
            }
            if (slot.key == key) {
                return index;
            }
            index = (index + 1) & mask_;
            ++distance;
        }
    }

    bool insertNoGrow(const Key& key, const Value& value) {
        std::size_t index = static_cast<std::size_t>(hash_(key)) & mask_;
        Slot carried;
        carried.key = key;
        carried.value = value;
        carried.distance = 0;
        bool inserted = true;

        for (;;) {
            Slot& slot = slots_[index];
            this->probe();
            if (slot.distance == kFree) {
                slot = std::move(carried);
                ++size_;
                return inserted;
            }
            if (slot.distance == carried.distance && slot.key == carried.key) {
                slot.value = carried.value; // same key: overwrite, size unchanged
                return false;
            }
            if (slot.distance < carried.distance) {
                // The resident is richer (closer to home): take its place and
                // carry it onward. This is the Robin Hood swap.
                std::swap(slot, carried);
                this->swapOp();
            }
            index = (index + 1) & mask_;
            ++carried.distance;
        }
    }

    void growTo(std::size_t newCapacity) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(newCapacity, Slot());
        mask_ = newCapacity - 1;
        size_ = 0;
        for (Slot& slot : old) {
            if (slot.distance != kFree) {
                insertNoGrow(slot.key, slot.value);
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t       size_ = 0;
    std::size_t       mask_ = 0;
    Hash              hash_;
};

} // namespace dsa
} // namespace neo
