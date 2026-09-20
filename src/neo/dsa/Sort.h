#pragma once

#include "neo/dsa/Instrumentation.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neo {
namespace dsa {

// Stable merge sort over index arrays, plus my own binary-search bounds.
//
// WHAT IT IS FOR
//   The ordered views the query engine ranges over: objects by diameter, H,
//   MOID, a, e, i; approaches by date, distance and velocity. Nothing is ever
//   sorted in place: the records stay put in the master vector and a view is a
//   permutation of std::uint32_t indices, so N views cost N * 4 bytes per entry
//   instead of N copies of a 392-byte record.
//
// COMPLEXITY (n = entries in the view)
//   mergeSort            O(n log n) comparisons, always: no input order makes
//                        it faster or slower, which is what makes it a fair
//                        baseline to measure against
//   space                one n-element scratch buffer, allocated once by the
//                        caller and reused for every level of the recursion
//   lowerBound/upperBound O(log n) comparisons, O(1) space
//   equalRange           O(log n)
//
// WHY MERGE SORT
//   * Stability is the point. Ties (equal diameters, equal dates, whole-degree
//     inclinations) are extremely common in this data, and a stable sort makes
//     every view deterministic: equal keys stay in record order, so two runs
//     produce byte-identical results and the oracle tests can compare exactly.
//     std::sort (introsort) is not stable and would shuffle ties unpredictably.
//   * Worst-case O(n log n) with no pathological input, unlike quicksort.
//   * The merge is sequential in memory, which suits index arrays.
//
// WHY THE INSERTION-SORT CUTOFF
//   Below ~16 elements the recursion and the copy in and out of the scratch
//   buffer cost more than the O(k^2) comparisons of an insertion sort on a run
//   that is already in cache. Insertion sort is itself stable (it only moves an
//   element past strictly greater ones), so the overall sort stays stable. The
//   cutoff also removes most of the recursive calls: for n = 42,666 it turns
//   ~5,300 leaf calls into ~2,700 straight-line loops.

constexpr std::size_t kInsertionSortCutoff = 16;

// --- insertion sort (stable) ------------------------------------------------

template <class Compare, class Counters = NullCounters>
void insertionSort(std::uint32_t* first, std::size_t n, const Compare& less, Counters& counters) {
    for (std::size_t i = 1; i < n; ++i) {
        const std::uint32_t value = first[i];
        std::size_t j = i;
        // Strictly-less comparison only, so equal elements are never swapped.
        while (j > 0) {
            counters.comparison();
            if (!less(value, first[j - 1])) {
                break;
            }
            first[j] = first[j - 1];
            counters.moveOp();
            --j;
        }
        first[j] = value;
    }
}

// --- merge sort (stable) ----------------------------------------------------

template <class Compare, class Counters>
void mergeRuns(std::uint32_t* first, std::size_t left, std::size_t middle, std::size_t right,
               const Compare& less, std::uint32_t* buffer, Counters& counters) {
    std::size_t i = left;
    std::size_t j = middle;
    std::size_t out = left;
    while (i < middle && j < right) {
        counters.comparison();
        // Take from the right half only when it is strictly smaller: on a tie
        // the left element wins, which is exactly what keeps the sort stable.
        if (less(first[j], first[i])) {
            buffer[out++] = first[j++];
        } else {
            buffer[out++] = first[i++];
        }
        counters.moveOp();
    }
    while (i < middle) {
        buffer[out++] = first[i++];
        counters.moveOp();
    }
    while (j < right) {
        buffer[out++] = first[j++];
        counters.moveOp();
    }
    for (std::size_t k = left; k < right; ++k) {
        first[k] = buffer[k];
    }
}

template <class Compare, class Counters>
void mergeSortRange(std::uint32_t* first, std::size_t left, std::size_t right, const Compare& less,
                    std::uint32_t* buffer, Counters& counters) {
    const std::size_t n = right - left;
    if (n <= 1) {
        return;
    }
    if (n <= kInsertionSortCutoff) {
        insertionSort(first + left, n, less, counters);
        return;
    }
    const std::size_t middle = left + n / 2;
    mergeSortRange(first, left, middle, less, buffer, counters);
    mergeSortRange(first, middle, right, less, buffer, counters);
    // Already ordered across the seam: skip the merge entirely. This is what
    // makes an already-sorted view cost O(n log n) comparisons but no moves.
    counters.comparison();
    if (!less(first[middle], first[middle - 1])) {
        return;
    }
    mergeRuns(first, left, middle, right, less, buffer, counters);
}

// Sorts `indices` with `less`, using one scratch buffer for every level.
template <class Compare, class Counters = NullCounters>
void mergeSort(std::vector<std::uint32_t>& indices, const Compare& less, std::vector<std::uint32_t>& buffer,
               Counters& counters) {
    if (indices.size() <= 1) {
        return;
    }
    buffer.resize(indices.size()); // one allocation, reused by every merge
    mergeSortRange(indices.data(), 0, indices.size(), less, buffer.data(), counters);
}

template <class Compare>
void mergeSort(std::vector<std::uint32_t>& indices, const Compare& less) {
    std::vector<std::uint32_t> buffer;
    NullCounters counters;
    mergeSort(indices, less, buffer, counters);
}

// --- binary search ----------------------------------------------------------
//
// Both bounds work on a sorted array of keys (the view keeps keys parallel to
// its index array, so a search touches one contiguous double array instead of
// chasing 42k records through memory).

// First position whose key is >= value.
template <class Counters = NullCounters>
std::size_t lowerBound(const double* keys, std::size_t n, double value, Counters& counters) {
    std::size_t low = 0;
    std::size_t count = n;
    while (count > 0) {
        const std::size_t step = count / 2;
        const std::size_t mid = low + step;
        counters.comparison();
        if (keys[mid] < value) {
            low = mid + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return low;
}

// First position whose key is > value.
template <class Counters = NullCounters>
std::size_t upperBound(const double* keys, std::size_t n, double value, Counters& counters) {
    std::size_t low = 0;
    std::size_t count = n;
    while (count > 0) {
        const std::size_t step = count / 2;
        const std::size_t mid = low + step;
        counters.comparison();
        if (!(value < keys[mid])) {
            low = mid + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return low;
}

inline std::size_t lowerBound(const double* keys, std::size_t n, double value) {
    NullCounters counters;
    return lowerBound(keys, n, value, counters);
}
inline std::size_t upperBound(const double* keys, std::size_t n, double value) {
    NullCounters counters;
    return upperBound(keys, n, value, counters);
}

// --- sorted view ------------------------------------------------------------

// A permutation of record indices ordered by one numeric key, with the keys
// kept alongside so a range query never dereferences a record until it has to.
//
// Records whose key is unknown are NOT in the view. That is the project rule
// made structural: "diameter > 100 m" cannot accidentally match an object with
// no measured diameter, because such an object is not in the diameter view at
// all. The count is kept so a query can report how many records it could not
// consider. NaN is refused the same way: it would make the ordering
// meaningless (NaN compares false against everything).
struct SortedView {
    std::vector<std::uint32_t> order; // record indices, ascending by key
    std::vector<double>        keys;  // keys[i] is the key of order[i]

    std::size_t excludedUnknown = 0;
    std::size_t excludedNaN = 0;

    std::size_t size() const { return order.size(); }
    bool        empty() const { return order.empty(); }

    // Indices of the entries whose key lies in [lo, hi], inclusive.
    // O(log n) to find the bounds, then the slice is contiguous.
    std::pair<std::size_t, std::size_t> range(double lo, double hi) const {
        if (order.empty() || !(lo <= hi)) {
            return {0, 0};
        }
        const std::size_t begin = lowerBound(keys.data(), keys.size(), lo);
        const std::size_t end = upperBound(keys.data(), keys.size(), hi);
        return {begin, end};
    }

    bool sortedAscending() const {
        for (std::size_t i = 1; i < keys.size(); ++i) {
            if (keys[i] < keys[i - 1]) {
                return false;
            }
        }
        return true;
    }
};

// Builds a view over `count` records. keyOf(i) returns the key of record i, or
// an empty optional when that record has no value for this key.
template <class KeyFn, class Counters = NullCounters>
SortedView buildSortedView(std::size_t count, const KeyFn& keyOf, Counters& counters) {
    SortedView view;
    view.order.reserve(count);
    std::vector<double> raw;
    raw.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::optional<double> key = keyOf(static_cast<std::uint32_t>(i));
        if (!key) {
            ++view.excludedUnknown;
            continue;
        }
        if (std::isnan(*key)) {
            ++view.excludedNaN;
            continue;
        }
        view.order.push_back(static_cast<std::uint32_t>(i));
        raw.push_back(*key);
    }

    // Sort the index array by (key, index). The index tiebreak is redundant for
    // a stable sort, but it states the intended order explicitly.
    std::vector<std::uint32_t> positions(view.order.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        positions[i] = static_cast<std::uint32_t>(i);
    }
    std::vector<std::uint32_t> buffer;
    const std::vector<std::uint32_t>& order = view.order;
    mergeSort(
        positions,
        [&raw, &order](std::uint32_t a, std::uint32_t b) {
            if (raw[a] != raw[b]) {
                return raw[a] < raw[b];
            }
            return order[a] < order[b];
        },
        buffer, counters);

    std::vector<std::uint32_t> sortedOrder(positions.size());
    view.keys.resize(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        sortedOrder[i] = view.order[positions[i]];
        view.keys[i] = raw[positions[i]];
    }
    view.order.swap(sortedOrder);
    return view;
}

template <class KeyFn>
SortedView buildSortedView(std::size_t count, const KeyFn& keyOf) {
    NullCounters counters;
    return buildSortedView(count, keyOf, counters);
}

} // namespace dsa
} // namespace neo
