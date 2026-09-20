#pragma once

#include "neo/dsa/Instrumentation.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace neo {
namespace dsa {

// Binary heap in an array, and the top-K query built on it.
//
// WHAT IT IS FOR
//   "The 10 closest approaches in this window", "the 20 fastest", "the largest
//   objects that match these filters". The answer is a handful of rows out of
//   tens of thousands, so sorting everything to read the first ten is waste.
//
// COMPLEXITY (n elements, k wanted)
//   push / pop           O(log n)
//   top                  O(1)
//   heapify(vector)      O(n), not O(n log n): the bound comes from the sum
//                        over levels, sum_{h} (n / 2^(h+1)) * h, which
//                        converges to n
//   topK over n items    O(n log k) time, O(k) space
//   space                one vector, no per-element allocation
//
// WHY A HEAP FOR TOP-K
//   Sorting the candidate set is O(n log n) and needs room for all of it.
//   A heap of exactly k elements keeps the *worst* of the current best k on
//   top, so each new candidate costs one comparison against that worst element
//   and only occasionally a O(log k) replacement. For n = 42,819 approaches and
//   k = 10 that is ~42.8k comparisons and a 10-element working set, against
//   ~600k comparisons and a full copy for a sort. The gap is what stage 7
//   measures against std::partial_sort and std::priority_queue.
//
// ORDERING
//   Compare is a strict "less" predicate and the heap is a MAX-heap: top() is
//   the greatest element under Compare. topK() passes its ranking predicate
//   straight in, because the greatest element under "ranks above" is the one
//   that ranks last: the worst of the k kept so far, and so the one to evict.

template <class T, class Compare = std::less<T>, class Counters = NullCounters>
class BinaryHeap : private Counters {
public:
    BinaryHeap() = default;
    explicit BinaryHeap(Compare compare) : compare_(std::move(compare)) {}

    // Heapifies an existing array in place: Floyd's algorithm, O(n).
    BinaryHeap(std::vector<T> items, Compare compare) : compare_(std::move(compare)), items_(std::move(items)) {
        heapify();
    }

    void push(const T& value) {
        items_.push_back(value);
        siftUp(items_.size() - 1);
    }

    const T& top() const { return items_.front(); }

    void pop() {
        items_.front() = items_.back(); // the last leaf moves to the root
        items_.pop_back();
        if (!items_.empty()) {
            siftDown(0);
        }
    }

    // Replaces the root and restores the heap: one sift-down instead of a
    // pop followed by a push, which is what the top-K loop wants.
    void replaceTop(const T& value) {
        items_.front() = value;
        siftDown(0);
    }

    bool        empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    void        clear() { items_.clear(); }
    void        reserve(std::size_t n) { items_.reserve(n); }

    const std::vector<T>& data() const { return items_; }
    // Takes the contents (no longer a heap afterwards).
    std::vector<T> release() { return std::move(items_); }

    // Every parent must compare >= both children.
    bool checkInvariants(std::string& error) const {
        for (std::size_t i = 1; i < items_.size(); ++i) {
            const std::size_t parent = (i - 1) / 2;
            if (compare_(items_[parent], items_[i])) {
                error = "heap order broken at index " + std::to_string(i);
                return false;
            }
        }
        return true;
    }

    const Counters& counters() const { return *this; }
    Counters& counters() { return *this; }

private:
    void heapify() {
        if (items_.size() < 2) {
            return;
        }
        // Leaves are already heaps, so start at the last internal node.
        std::size_t i = items_.size() / 2;
        while (i > 0) {
            --i;
            siftDown(i);
        }
    }

    void siftUp(std::size_t index) {
        T value = std::move(items_[index]);
        while (index > 0) {
            const std::size_t parent = (index - 1) / 2;
            this->comparison();
            if (!compare_(items_[parent], value)) {
                break;
            }
            items_[index] = std::move(items_[parent]);
            this->moveOp();
            index = parent;
        }
        items_[index] = std::move(value);
    }

    void siftDown(std::size_t index) {
        const std::size_t n = items_.size();
        T value = std::move(items_[index]);
        for (;;) {
            std::size_t child = 2 * index + 1;
            if (child >= n) {
                break;
            }
            if (child + 1 < n) {
                this->comparison();
                if (compare_(items_[child], items_[child + 1])) {
                    ++child; // the greater child
                }
            }
            this->comparison();
            if (!compare_(value, items_[child])) {
                break;
            }
            items_[index] = std::move(items_[child]);
            this->moveOp();
            index = child;
        }
        items_[index] = std::move(value);
    }

    Compare        compare_;
    std::vector<T> items_;
};

// --- top-K ------------------------------------------------------------------

// The k best of `count` candidates, best first.
//
// better(a, b) is a strict "a ranks above b" predicate (closest, fastest,
// largest: whatever the query asked for). The heap inside holds the current
// best k ordered so that the WORST of them is on top, which is why it is
// constructed with the predicate inverted.
//
// O(n log k) time, O(k) space. The result is fully ordered best-first, which
// costs the final k log k sift-downs.
template <class Better, class Counters = NullCounters>
std::vector<std::uint32_t> topK(const std::uint32_t* candidates, std::size_t count, std::size_t k,
                                const Better& better, Counters& counters) {
    std::vector<std::uint32_t> result;
    if (k == 0 || count == 0) {
        return result;
    }
    // The heap is a max-heap under its comparator, and the maximum under
    // "ranks above" is the element that ranks LAST. Feeding it `better`
    // directly therefore puts the worst of the current best k on top, which is
    // the one a new candidate has to beat.
    const auto worstOnTop = [&better, &counters](std::uint32_t a, std::uint32_t b) {
        counters.comparison();
        return better(a, b);
    };
    // The comparator already counts its own comparisons, so the heap itself
    // uses the null policy (a reference cannot be a base class).
    BinaryHeap<std::uint32_t, decltype(worstOnTop)> heap(worstOnTop);
    heap.reserve(k < count ? k : count);

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t candidate = candidates[i];
        if (heap.size() < k) {
            heap.push(candidate);
            continue;
        }
        // One comparison against the worst kept element rejects most candidates.
        counters.comparison();
        if (better(candidate, heap.top())) {
            heap.replaceTop(candidate);
        }
    }

    result = heap.release();
    // A heap is not a sorted array, so the k survivors are ordered best-first
    // here. Insertion sort suits it: k is small (typically 10-50), and it keeps
    // this file independent of the sort module.
    for (std::size_t i = 1; i < result.size(); ++i) {
        const std::uint32_t value = result[i];
        std::size_t j = i;
        while (j > 0) {
            counters.comparison();
            if (!better(value, result[j - 1])) {
                break;
            }
            result[j] = result[j - 1];
            --j;
        }
        result[j] = value;
    }
    return result;
}

template <class Better>
std::vector<std::uint32_t> topK(const std::vector<std::uint32_t>& candidates, std::size_t k, const Better& better) {
    NullCounters counters;
    return topK(candidates.data(), candidates.size(), k, better, counters);
}

} // namespace dsa
} // namespace neo
