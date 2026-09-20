// DSA tests: edge cases, differential fuzzing against the std:: equivalents
// with fixed seeds, and a pass over the real dataset when data/neo.db exists.
// No network.
//
// The fuzz tests are the point: every one of my structures is driven through
// hundreds of thousands of random operations alongside the std:: container it
// replaces, and the two must agree at every step. Invariant checkers run
// periodically during the fuzz, so a structure that breaks internally is caught
// where it breaks, not later when a query returns the wrong answer.

#include "neo/dsa/AvlTree.h"
#include "neo/dsa/BinaryHeap.h"
#include "neo/dsa/BucketIndex.h"
#include "neo/dsa/HashMap.h"
#include "neo/dsa/Sort.h"
#include "neo/model/Dataset.h"
#include "neo/storage/Database.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <optional>
#include <filesystem>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

std::string num(std::size_t v) { return std::to_string(v); }

// Deterministic generator: the same stream on every compiler and every run, so
// a failure is reproducible from the seed alone.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n); }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

private:
    std::uint64_t state_;
};

std::string keyFor(std::uint32_t n) {
    // Designation-shaped keys, so the string hashing is exercised the way the
    // real index uses it ("2020 AB12", "433").
    if (n % 3 == 0) {
        return std::to_string(n);
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u %c%c%u", 1900 + (n % 200), static_cast<char>('A' + (n % 26)),
                  static_cast<char>('A' + ((n / 26) % 26)), n % 100);
    return buf;
}

// --- HashMap ---------------------------------------------------------------

void testHashMapBasics() {
    std::printf("[hashmap] edge cases\n");
    neo::dsa::HashMap<std::string, std::uint32_t> map;
    std::string error;

    check(map.size() == 0 && map.empty(), "a new map is empty");
    check(map.find("nothing") == nullptr, "find on an empty map returns null");
    check(!map.erase("nothing"), "erase on an empty map returns false");
    check(map.checkInvariants(error), "an empty map is structurally sound", error);

    check(map.insert("433", 7), "the first insert is new");
    check(map.size() == 1, "size is 1");
    check(map.find("433") != nullptr && *map.find("433") == 7, "the value is found");
    check(!map.insert("433", 9), "re-inserting the same key overwrites");
    check(map.size() == 1 && *map.find("433") == 9, "and does not grow the map");
    check(map.erase("433") && map.size() == 0, "erase removes it");
    check(map.find("433") == nullptr, "and it is gone");
    check(map.checkInvariants(error), "the map is sound after erase-to-empty", error);

    // Growth: insert enough to force several rehashes, then check every key.
    for (std::uint32_t i = 0; i < 1000; ++i) {
        map.insert(keyFor(i), i);
    }
    check(map.size() == 1000, "1000 entries", num(map.size()));
    check(map.loadFactor() <= 0.75, "the load factor stays under 0.75");
    bool allFound = true;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        const std::uint32_t* value = map.find(keyFor(i));
        allFound = allFound && value != nullptr && *value == i;
    }
    check(allFound, "every key survives the rehashes");
    check(map.checkInvariants(error), "invariants hold after growth", error);

    // Erase everything: backward-shift deletion must leave a usable table.
    for (std::uint32_t i = 0; i < 1000; ++i) {
        map.erase(keyFor(i));
    }
    check(map.size() == 0, "erase-until-empty");
    check(map.checkInvariants(error), "and the table is still sound", error);
    map.insert("after", 1);
    check(map.find("after") != nullptr, "the table still works after being emptied");

    // A pathological hash: every key lands in the same slot.
    struct ConstantHash {
        std::uint64_t operator()(std::uint32_t) const { return 42; }
    };
    neo::dsa::HashMap<std::uint32_t, std::uint32_t, ConstantHash> collide;
    for (std::uint32_t i = 0; i < 64; ++i) {
        collide.insert(i, i * 10);
    }
    bool collideOk = true;
    for (std::uint32_t i = 0; i < 64; ++i) {
        const std::uint32_t* v = collide.find(i);
        collideOk = collideOk && v != nullptr && *v == i * 10;
    }
    check(collideOk, "64 keys that all hash to one slot are all findable");
    check(collide.checkInvariants(error), "even then the invariants hold", error);
    check(collide.erase(30) && collide.find(30) == nullptr && collide.find(31) != nullptr,
          "erasing from the middle of a long run keeps the rest reachable");

    // Probe statistics are the numbers stage 7 reports.
    const neo::dsa::ProbeStats stats = map.probeStats();
    check(stats.size == map.size() && stats.capacity == map.capacity(), "probe stats report size and capacity");
    check(stats.meanProbe >= 0.0 && stats.maxProbe < map.capacity(), "probe stats are in range");
}

void testHashMapFuzz() {
    std::printf("[hashmap] 200k random ops against std::unordered_map\n");
    neo::dsa::HashMap<std::string, std::uint32_t, neo::dsa::DefaultHash<std::string>, neo::dsa::LiveCounters> mine;
    std::unordered_map<std::string, std::uint32_t> reference;
    Rng rng(0xA5A5A5A5u);
    std::string error;
    bool agreed = true;
    bool sound = true;
    constexpr std::size_t kOps = 200000;

    for (std::size_t op = 0; op < kOps && agreed && sound; ++op) {
        const std::uint32_t key = rng.below(4000); // a key space small enough to collide often
        const std::string k = keyFor(key);
        const std::uint32_t roll = rng.below(100);
        if (roll < 45) {
            const std::uint32_t value = rng.below(1000000);
            const bool mineNew = mine.insert(k, value);
            const bool refNew = reference.insert_or_assign(k, value).second;
            agreed = agreed && mineNew == refNew;
        } else if (roll < 70) {
            const std::uint32_t* mineValue = mine.find(k);
            const auto it = reference.find(k);
            const bool refHas = it != reference.end();
            agreed = agreed && (mineValue != nullptr) == refHas;
            if (mineValue != nullptr && refHas) {
                agreed = agreed && *mineValue == it->second;
            }
        } else if (roll < 95) {
            agreed = agreed && mine.erase(k) == (reference.erase(k) != 0);
        } else {
            mine.clear();
            reference.clear();
        }
        agreed = agreed && mine.size() == reference.size();
        if (op % 5000 == 0) {
            sound = mine.checkInvariants(error);
        }
    }
    check(agreed, "my map agrees with std::unordered_map on every operation");
    check(sound, "invariants held throughout the fuzz", error);

    // Final full comparison.
    bool sameContents = mine.size() == reference.size();
    mine.forEach([&](const std::string& k, std::uint32_t v) {
        const auto it = reference.find(k);
        sameContents = sameContents && it != reference.end() && it->second == v;
    });
    check(sameContents, "and holds exactly the same entries at the end");
    check(mine.counters().probes > 0, "the instrumented build counted probes",
          num(static_cast<std::size_t>(mine.counters().probes)));
}

// --- sorting ---------------------------------------------------------------

struct StableItem {
    double        key;
    std::uint32_t original;
};

void testSortBasics() {
    std::printf("[sort] edge cases, stability and binary search\n");
    std::vector<std::uint32_t> empty;
    neo::dsa::mergeSort(empty, [](std::uint32_t a, std::uint32_t b) { return a < b; });
    check(empty.empty(), "sorting an empty array is a no-op");

    std::vector<std::uint32_t> one{5};
    neo::dsa::mergeSort(one, [](std::uint32_t a, std::uint32_t b) { return a < b; });
    check(one.size() == 1 && one[0] == 5, "sorting one element is a no-op");

    // Already sorted, reverse sorted, all equal: the three shapes that break
    // naive implementations.
    for (int shape = 0; shape < 3; ++shape) {
        std::vector<std::uint32_t> data(1000);
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = shape == 0 ? static_cast<std::uint32_t>(i)
                      : shape == 1 ? static_cast<std::uint32_t>(data.size() - i)
                                   : 7u;
        }
        std::vector<std::uint32_t> expected = data;
        std::stable_sort(expected.begin(), expected.end());
        neo::dsa::mergeSort(data, [](std::uint32_t a, std::uint32_t b) { return a < b; });
        check(data == expected, shape == 0 ? "already sorted" : shape == 1 ? "reverse sorted" : "all equal");
    }

    // Stability, stated as a property: equal keys keep their original order.
    Rng rng(0xBEEF1234u);
    std::vector<StableItem> items(2000);
    for (std::size_t i = 0; i < items.size(); ++i) {
        items[i].key = static_cast<double>(rng.below(20)); // many ties on purpose
        items[i].original = static_cast<std::uint32_t>(i);
    }
    std::vector<std::uint32_t> order(items.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    std::vector<std::uint32_t> reference = order;
    neo::dsa::LiveCounters counters;
    std::vector<std::uint32_t> buffer;
    neo::dsa::mergeSort(
        order, [&items](std::uint32_t a, std::uint32_t b) { return items[a].key < items[b].key; }, buffer, counters);
    std::stable_sort(reference.begin(), reference.end(),
                     [&items](std::uint32_t a, std::uint32_t b) { return items[a].key < items[b].key; });
    check(order == reference, "my merge sort matches std::stable_sort exactly, ties included");
    bool stable = true;
    for (std::size_t i = 1; i < order.size(); ++i) {
        if (items[order[i - 1]].key == items[order[i]].key) {
            stable = stable && order[i - 1] < order[i];
        }
    }
    check(stable, "equal keys stay in their original relative order");
    check(counters.comparisons > 0 && counters.moves > 0, "the sort counted its comparisons and moves");

    // Binary search bounds, including the empty and all-equal cases.
    const std::vector<double> keys{1.0, 2.0, 2.0, 2.0, 5.0, 9.0};
    check(neo::dsa::lowerBound(keys.data(), keys.size(), 2.0) == 1, "lowerBound finds the first match");
    check(neo::dsa::upperBound(keys.data(), keys.size(), 2.0) == 4, "upperBound finds one past the last");
    check(neo::dsa::lowerBound(keys.data(), keys.size(), 0.0) == 0, "lowerBound below everything");
    check(neo::dsa::lowerBound(keys.data(), keys.size(), 100.0) == keys.size(), "lowerBound above everything");
    check(neo::dsa::upperBound(keys.data(), keys.size(), 100.0) == keys.size(), "upperBound above everything");
    check(neo::dsa::lowerBound(nullptr, 0, 1.0) == 0, "bounds on an empty array");
    const std::vector<double> allEqual(64, 3.0);
    check(neo::dsa::lowerBound(allEqual.data(), allEqual.size(), 3.0) == 0 &&
              neo::dsa::upperBound(allEqual.data(), allEqual.size(), 3.0) == allEqual.size(),
          "bounds over an all-equal array cover the whole range");
}

void testSortedView() {
    std::printf("[sort] sorted views exclude unknown keys and NaN\n");
    std::vector<std::optional<double>> values{1.5, std::nullopt, 0.5, std::numeric_limits<double>::quiet_NaN(),
                                              2.5, std::nullopt, 0.5};
    const neo::dsa::SortedView view =
        neo::dsa::buildSortedView(values.size(), [&values](std::uint32_t i) { return values[i]; });
    check(view.size() == 4, "only records with a usable key are in the view", num(view.size()));
    check(view.excludedUnknown == 2, "unknown values are counted, not sorted as 0", num(view.excludedUnknown));
    check(view.excludedNaN == 1, "NaN is refused", num(view.excludedNaN));
    check(view.sortedAscending(), "the view is ascending");
    check(view.order[0] == 2 && view.order[1] == 6, "ties are broken by record index (stable)");

    const std::pair<std::size_t, std::size_t> range = view.range(0.5, 1.5);
    check(range.first == 0 && range.second == 3, "an inclusive range covers both ends",
          num(range.first) + ".." + num(range.second));
    const std::pair<std::size_t, std::size_t> none = view.range(10.0, 20.0);
    check(none.first == none.second, "a range past the end is empty");
    const std::pair<std::size_t, std::size_t> inverted = view.range(5.0, 1.0);
    check(inverted.first == inverted.second, "an inverted range is empty, not undefined");

    // A view over nothing at all.
    const neo::dsa::SortedView emptyView =
        neo::dsa::buildSortedView(0, [](std::uint32_t) { return std::optional<double>(); });
    check(emptyView.empty() && emptyView.range(0.0, 1.0).first == 0, "an empty view answers ranges safely");
}

// --- heap ------------------------------------------------------------------

void testHeapBasics() {
    std::printf("[heap] order, heapify and top-K edge cases\n");
    std::string error;
    neo::dsa::BinaryHeap<int> heap;
    check(heap.empty(), "a new heap is empty");
    heap.push(5);
    check(heap.top() == 5 && heap.size() == 1, "one element");
    heap.push(9);
    heap.push(1);
    check(heap.top() == 9, "the maximum is on top");
    check(heap.checkInvariants(error), "heap order holds", error);
    heap.pop();
    check(heap.top() == 5, "pop removes the maximum");

    // heapify on an arbitrary array, including all-equal.
    Rng rng(0xC0FFEEu);
    std::vector<int> values(1000);
    for (int& v : values) {
        v = static_cast<int>(rng.below(100));
    }
    neo::dsa::BinaryHeap<int> built(values, std::less<int>());
    check(built.checkInvariants(error), "heapify produces a valid heap", error);
    check(built.size() == 1000, "heapify keeps every element");
    std::vector<int> drained;
    while (!built.empty()) {
        drained.push_back(built.top());
        built.pop();
    }
    check(std::is_sorted(drained.begin(), drained.end(), std::greater<int>()), "draining yields descending order");

    const std::vector<int> equal(500, 7);
    neo::dsa::BinaryHeap<int> flat(equal, std::less<int>());
    check(flat.checkInvariants(error), "an all-equal array heapifies", error);

    // top-K edge cases.
    std::vector<std::uint32_t> candidates{4, 1, 3, 2, 0};
    const auto better = [](std::uint32_t a, std::uint32_t b) { return a < b; }; // smaller ranks higher
    check(neo::dsa::topK(candidates, 0, better).empty(), "k = 0 returns nothing");
    const std::vector<std::uint32_t> all = neo::dsa::topK(candidates, 99, better);
    check(all.size() == candidates.size(), "k larger than the input returns everything");
    check(std::is_sorted(all.begin(), all.end()), "and it is ordered best-first");
    const std::vector<std::uint32_t> three = neo::dsa::topK(candidates, 3, better);
    check(three.size() == 3 && three[0] == 0 && three[1] == 1 && three[2] == 2, "top 3 are the three smallest");
    const std::vector<std::uint32_t> empty;
    check(neo::dsa::topK(empty, 5, better).empty(), "top-K over nothing is empty");
}

void testHeapFuzz() {
    std::printf("[heap] 200k random ops against std::priority_queue, top-K against std::partial_sort\n");
    Rng rng(0x1234ABCDu);
    neo::dsa::BinaryHeap<int, std::less<int>, neo::dsa::LiveCounters> mine;
    std::priority_queue<int> reference;
    bool agreed = true;
    std::string error;
    bool sound = true;
    constexpr std::size_t kOps = 200000;

    for (std::size_t op = 0; op < kOps && agreed && sound; ++op) {
        if (reference.empty() || rng.below(100) < 60) {
            const int value = static_cast<int>(rng.below(10000));
            mine.push(value);
            reference.push(value);
        } else {
            agreed = agreed && mine.top() == reference.top();
            mine.pop();
            reference.pop();
        }
        agreed = agreed && mine.size() == reference.size();
        if (!reference.empty()) {
            agreed = agreed && mine.top() == reference.top();
        }
        if (op % 10000 == 0) {
            sound = mine.checkInvariants(error);
        }
    }
    check(agreed, "my heap agrees with std::priority_queue at every step");
    check(sound, "heap invariants held throughout", error);

    // top-K against partial_sort over random keys, with ties.
    bool topKAgreed = true;
    for (int round = 0; round < 40 && topKAgreed; ++round) {
        const std::size_t n = 1 + rng.below(3000);
        std::vector<double> keys(n);
        for (double& key : keys) {
            key = static_cast<double>(rng.below(200)); // ties everywhere
        }
        std::vector<std::uint32_t> candidates(n);
        for (std::size_t i = 0; i < n; ++i) {
            candidates[i] = static_cast<std::uint32_t>(i);
        }
        const std::size_t k = 1 + rng.below(static_cast<std::uint32_t>(n));
        // Ranking must be a strict weak ordering with no ties, or "the top k"
        // is ambiguous; index breaks the tie, exactly as a query would.
        const auto better = [&keys](std::uint32_t a, std::uint32_t b) {
            if (keys[a] != keys[b]) {
                return keys[a] < keys[b];
            }
            return a < b;
        };
        const std::vector<std::uint32_t> mineTop = neo::dsa::topK(candidates, k, better);
        std::vector<std::uint32_t> expected = candidates;
        std::partial_sort(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(k), expected.end(), better);
        expected.resize(k);
        topKAgreed = mineTop == expected;
    }
    check(topKAgreed, "top-K matches std::partial_sort over 40 random rounds");
}

// --- AVL -------------------------------------------------------------------

void testAvlBasics() {
    std::printf("[avl] edge cases, rotations and range queries\n");
    neo::dsa::AvlTree<double> tree;
    std::string error;
    check(tree.empty() && tree.height() == 0, "a new tree is empty");
    check(tree.checkInvariants(error), "an empty tree is sound", error);
    check(!tree.erase(1.0, 0), "erasing from an empty tree returns false");

    check(tree.insert(1.0, 0) && tree.size() == 1, "one insert");
    check(!tree.insert(1.0, 0), "the same (key, payload) is not inserted twice");
    check(tree.insert(1.0, 1), "the same key with a different payload is a new entry");
    check(tree.size() == 2, "duplicate keys coexist", num(tree.size()));
    check(tree.checkInvariants(error), "invariants hold with duplicate keys", error);

    // The four rotation cases, each built deliberately.
    struct Case {
        const char* name;
        double      keys[3];
    };
    const Case cases[4] = {{"LL (right rotation)", {3, 2, 1}},
                           {"RR (left rotation)", {1, 2, 3}},
                           {"LR (left then right)", {3, 1, 2}},
                           {"RL (right then left)", {1, 3, 2}}};
    for (const Case& c : cases) {
        neo::dsa::AvlTree<double, neo::dsa::LiveCounters> t;
        for (const double key : c.keys) {
            t.insert(key, 0);
        }
        check(t.checkInvariants(error), c.name, error);
        check(t.height() == 2, "the tree is rebalanced to height 2");
        check(t.counters().rotations >= 1, "a rotation happened");
    }

    // Ascending inserts: without rebalancing this would be a 1000-deep list.
    neo::dsa::AvlTree<double> ascending;
    for (int i = 0; i < 1000; ++i) {
        ascending.insert(static_cast<double>(i), static_cast<std::uint32_t>(i));
    }
    check(ascending.checkInvariants(error), "1000 ascending inserts stay balanced", error);
    check(ascending.height() <= 15, "height stays near log2(n), not n", std::to_string(ascending.height()));

    // Range queries.
    std::vector<std::uint32_t> visited;
    ascending.range(10.0, 20.0, [&visited](double, std::uint32_t payload) { visited.push_back(payload); });
    check(visited.size() == 11, "an inclusive range returns both endpoints", num(visited.size()));
    check(visited.front() == 10 && visited.back() == 20, "and the right ones");
    visited.clear();
    ascending.range(-5.0, -1.0, [&visited](double, std::uint32_t p) { visited.push_back(p); });
    check(visited.empty(), "a range below everything is empty");
    visited.clear();
    ascending.range(20.0, 10.0, [&visited](double, std::uint32_t p) { visited.push_back(p); });
    check(visited.empty(), "an inverted range is empty");
    visited.clear();
    const std::size_t all = ascending.range(-1e9, 1e9, [&visited](double, std::uint32_t p) { visited.push_back(p); });
    check(all == 1000 && visited.size() == 1000, "a range covering everything returns everything");

    // Erase until empty, checking the invariants as the tree shrinks.
    bool soundThroughout = true;
    for (int i = 0; i < 1000; ++i) {
        ascending.erase(static_cast<double>(i), static_cast<std::uint32_t>(i));
        if (i % 100 == 0) {
            soundThroughout = soundThroughout && ascending.checkInvariants(error);
        }
    }
    check(ascending.empty(), "erase-until-empty leaves an empty tree");
    check(soundThroughout, "invariants held while shrinking", error);
    // The node pool must be reused rather than grown again.
    ascending.insert(1.0, 1);
    check(ascending.size() == 1 && ascending.checkInvariants(error), "the tree is reusable after emptying", error);
}

void testAvlFuzz() {
    std::printf("[avl] 200k random ops against std::multimap, with range checks\n");
    neo::dsa::AvlTree<double, neo::dsa::LiveCounters> mine;
    std::map<std::pair<double, std::uint32_t>, bool> reference; // the same (key, payload) set
    Rng rng(0x5EED0001u);
    bool agreed = true;
    bool sound = true;
    std::string error;
    constexpr std::size_t kOps = 200000;

    for (std::size_t op = 0; op < kOps && agreed && sound; ++op) {
        const double key = static_cast<double>(rng.below(500)); // many duplicate keys
        const std::uint32_t payload = rng.below(50);
        const std::uint32_t roll = rng.below(100);
        if (roll < 50) {
            const bool mineNew = mine.insert(key, payload);
            const bool refNew = reference.emplace(std::make_pair(key, payload), true).second;
            agreed = agreed && mineNew == refNew;
        } else if (roll < 70) {
            agreed = agreed && mine.contains(key, payload) == (reference.count({key, payload}) != 0);
        } else if (roll < 95) {
            const bool mineGone = mine.erase(key, payload);
            const bool refGone = reference.erase({key, payload}) != 0;
            agreed = agreed && mineGone == refGone;
        } else {
            // Range query against the reference's equivalent slice.
            const double lo = static_cast<double>(rng.below(500));
            const double hi = lo + static_cast<double>(rng.below(50));
            std::vector<std::pair<double, std::uint32_t>> mineRange;
            mine.range(lo, hi, [&mineRange](double k, std::uint32_t p) { mineRange.emplace_back(k, p); });
            std::vector<std::pair<double, std::uint32_t>> refRange;
            for (auto it = reference.lower_bound({lo, 0}); it != reference.end() && it->first.first <= hi; ++it) {
                refRange.push_back(it->first);
            }
            agreed = agreed && mineRange == refRange;
        }
        agreed = agreed && mine.size() == reference.size();
        if (op % 5000 == 0) {
            sound = mine.checkInvariants(error);
        }
    }
    check(agreed, "my AVL tree agrees with the std:: reference on every operation");
    check(sound, "AVL invariants held throughout the fuzz", error);

    // Final in-order comparison.
    std::vector<std::pair<double, std::uint32_t>> mineAll;
    mine.inOrder([&mineAll](double k, std::uint32_t p) { mineAll.emplace_back(k, p); });
    std::vector<std::pair<double, std::uint32_t>> refAll;
    for (const auto& entry : reference) {
        refAll.push_back(entry.first);
    }
    check(mineAll == refAll, "in-order traversal matches the reference exactly");
    check(mine.counters().rotations > 0, "rotations were counted",
          num(static_cast<std::size_t>(mine.counters().rotations)));
}

// --- buckets ---------------------------------------------------------------

neo::Dataset syntheticDataset(std::size_t objects, std::size_t approachesPerObject, Rng& rng) {
    neo::Dataset dataset;
    std::vector<neo::Asteroid> list(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        list[i].pdes = keyFor(static_cast<std::uint32_t>(i));
        list[i].orbital.eccentricity = rng.unit();
        const std::uint32_t roll = rng.below(10);
        if (roll < 3) {
            list[i].physical.diameterKm = 0.002 + rng.unit() * 3.0; // measured
        } else if (roll < 7) {
            list[i].physical.absoluteMagnitudeH = 15.0 + rng.unit() * 12.0; // estimate only
        }
        // The rest have neither: they belong in the unknown bucket.
    }
    dataset.setObjects(std::move(list));

    std::vector<neo::CloseApproach> approaches;
    for (std::size_t i = 0; i < objects; ++i) {
        for (std::size_t k = 0; k < approachesPerObject; ++k) {
            neo::CloseApproach a;
            a.objectIndex = static_cast<std::uint32_t>(i);
            a.jdTdb = 2433282.5 + rng.unit() * 73050.0; // 1950..2150
            a.distanceAU = rng.unit() * 0.05;
            a.relVelocityKms = 2.0 + rng.unit() * 30.0;
            approaches.push_back(a);
        }
    }
    dataset.setApproaches(std::move(approaches));
    return dataset;
}

void testBuckets() {
    std::printf("[buckets] size classes and year buckets\n");
    check(neo::dsa::sizeBucketOf(0.005) == neo::dsa::SizeBucket::Under10m, "5 m is under 10 m");
    check(neo::dsa::sizeBucketOf(0.01) == neo::dsa::SizeBucket::From10To50m, "10 m is the lower edge of 10-50 m");
    check(neo::dsa::sizeBucketOf(0.049) == neo::dsa::SizeBucket::From10To50m, "49 m is in 10-50 m");
    check(neo::dsa::sizeBucketOf(0.05) == neo::dsa::SizeBucket::From50To140m, "50 m is the lower edge of 50-140 m");
    check(neo::dsa::sizeBucketOf(0.14) == neo::dsa::SizeBucket::From140mTo1km, "140 m starts the PHA-scale class");
    check(neo::dsa::sizeBucketOf(1.0) == neo::dsa::SizeBucket::Over1km, "1 km is over 1 km");
    check(neo::dsa::sizeBucketOf(25.0) == neo::dsa::SizeBucket::Over1km, "Eros-sized is over 1 km");

    Rng rng(0x512E0001u);
    const neo::Dataset dataset = syntheticDataset(2000, 2, rng);
    std::string error;

    neo::dsa::SizeBucketIndex measured;
    measured.build(dataset, neo::dsa::DiameterPolicy::MeasuredOnly);
    check(measured.checkInvariants(dataset, error), "the measured-only index is sound", error);

    neo::dsa::SizeBucketIndex estimated;
    estimated.build(dataset, neo::dsa::DiameterPolicy::MeasuredOrEstimate);
    check(estimated.checkInvariants(dataset, error), "the measured-or-estimate index is sound", error);

    // The policy decides the buckets, which is why an index carries one.
    check(estimated.count(neo::dsa::SizeBucket::Unknown) < measured.count(neo::dsa::SizeBucket::Unknown),
          "allowing estimates moves objects out of the unknown bucket");
    std::size_t total = 0;
    for (std::size_t b = 0; b < neo::dsa::kSizeBucketCount; ++b) {
        total += measured.count(static_cast<neo::dsa::SizeBucket>(b));
    }
    check(total == dataset.objectCount(), "every object is in exactly one bucket", num(total));

    // Cross-check one bucket against a linear scan.
    std::vector<std::uint32_t> expected;
    for (std::uint32_t i = 0; i < dataset.records().size(); ++i) {
        const std::optional<double> d = dataset.records()[i].object.physical.diameterKm;
        if (d && neo::dsa::sizeBucketOf(*d) == neo::dsa::SizeBucket::Over1km) {
            expected.push_back(i);
        }
    }
    const std::vector<std::uint32_t> got(measured.begin(neo::dsa::SizeBucket::Over1km),
                                         measured.end(neo::dsa::SizeBucket::Over1km));
    check(got == expected, "a bucket matches a linear scan exactly");

    neo::dsa::YearBucketIndex years;
    years.build(dataset);
    check(years.checkInvariants(dataset, error), "the year index is sound", error);
    check(years.size() == dataset.approachCount(), "every approach is filed");
    std::size_t counted = 0;
    for (int year = years.firstYear(); year <= years.lastYear(); ++year) {
        counted += years.count(year);
    }
    check(counted == dataset.approachCount(), "the year buckets cover everything", num(counted));
    check(years.count(1800) == 0 && years.count(3000) == 0, "years outside the data are empty, not out of bounds");
    const std::size_t decade = years.forEachInYears(2030, 2039, [](std::uint32_t) {});
    std::size_t decadeExpected = 0;
    for (const neo::CloseApproach& a : dataset.approaches()) {
        const int year = neo::dsa::yearOfJulianDate(a.jdTdb);
        decadeExpected += year >= 2030 && year <= 2039 ? 1u : 0u;
    }
    check(decade == decadeExpected, "a decade range matches a linear scan", num(decade));

    neo::dsa::YearBucketIndex emptyYears;
    const neo::Dataset emptyDataset;
    emptyYears.build(emptyDataset);
    check(emptyYears.size() == 0 && emptyYears.count(2000) == 0, "an empty dataset yields an empty year index");
}

// --- the real dataset -------------------------------------------------------

void testRealDataset() {
    std::printf("[real] the structures over data/neo.db, if it exists\n");
    const char* candidates[] = {"data/neo.db", "../data/neo.db", "../../data/neo.db"};
    std::string path;
    for (const char* candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            path = candidate;
            break;
        }
    }
    if (path.empty()) {
        std::printf("  skipped: no neo.db (run neo_ingest to create one)\n");
        return;
    }

    neo::Dataset dataset;
    neo::DatabaseMeta meta;
    const neo::DbStatus status = neo::loadDatabase(path, dataset, meta);
    if (!status) {
        std::printf("  skipped: %s\n", status.error.c_str());
        return;
    }
    std::printf("  %zu objects, %zu approaches from %s\n", dataset.objectCount(), dataset.approachCount(),
                path.c_str());
    std::string error;

    // The designation index, exactly as stage 6 will build it.
    neo::dsa::HashMap<std::string, std::uint32_t> byDesignation(dataset.objectCount());
    for (std::uint32_t i = 0; i < dataset.records().size(); ++i) {
        byDesignation.insert(dataset.records()[i].object.pdes, i);
    }
    check(byDesignation.size() == dataset.objectCount(), "every designation is indexed");
    check(byDesignation.checkInvariants(error), "the real index is structurally sound", error);
    bool resolves = true;
    for (std::uint32_t i = 0; i < dataset.records().size(); ++i) {
        const std::uint32_t* found = byDesignation.find(dataset.records()[i].object.pdes);
        resolves = resolves && found != nullptr && *found == i;
    }
    check(resolves, "every designation resolves to its own record");
    const neo::dsa::ProbeStats stats = byDesignation.probeStats();
    std::printf("  hash: %zu entries, capacity %zu, load %.2f, mean probe %.2f, max probe %zu\n", stats.size,
                stats.capacity, stats.loadFactor, stats.meanProbe, stats.maxProbe);
    check(stats.maxProbe < 64, "Robin Hood keeps the worst probe short on real data", num(stats.maxProbe));

    // A sorted view over a field most objects lack: the exclusion count is the
    // honest answer to "how many could I not consider".
    const neo::dsa::SortedView byDiameter =
        neo::dsa::buildSortedView(dataset.objectCount(), [&dataset](std::uint32_t i) {
            return dataset.records()[i].object.physical.diameterKm;
        });
    check(byDiameter.sortedAscending(), "the diameter view is ordered");
    check(byDiameter.size() + byDiameter.excludedUnknown == dataset.objectCount(),
          "measured diameters plus unknowns account for every object");
    std::printf("  diameter view: %zu with a measured value, %zu unknown\n", byDiameter.size(),
                byDiameter.excludedUnknown);

    // An AVL index over approach dates, then a decade range cross-checked
    // against a linear scan.
    neo::dsa::AvlTree<double> byDate;
    byDate.reserve(dataset.approachCount());
    for (std::uint32_t i = 0; i < dataset.approaches().size(); ++i) {
        byDate.insert(dataset.approaches()[i].jdTdb, i);
    }
    check(byDate.size() == dataset.approachCount(), "every approach is in the date index");
    check(byDate.checkInvariants(error), "the real AVL index is sound", error);
    std::printf("  avl: %zu nodes, height %d\n", byDate.size(), byDate.height());

    const double lo = 2462502.5; // 2030-01-01
    const double hi = 2466154.5; // 2040-01-01
    std::size_t fromTree = 0;
    byDate.range(lo, hi, [&fromTree](double, std::uint32_t) { ++fromTree; });
    std::size_t fromScan = 0;
    for (const neo::CloseApproach& a : dataset.approaches()) {
        fromScan += a.jdTdb >= lo && a.jdTdb <= hi ? 1u : 0u;
    }
    check(fromTree == fromScan, "the 2030s range matches a linear scan", num(fromTree) + " vs " + num(fromScan));
    std::printf("  approaches in the 2030s: %zu\n", fromTree);

    // Top-10 closest, against a full sort of the same data.
    std::vector<std::uint32_t> all(dataset.approachCount());
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = static_cast<std::uint32_t>(i);
    }
    const auto closer = [&dataset](std::uint32_t a, std::uint32_t b) {
        const double da = dataset.approaches()[a].distanceAU;
        const double db = dataset.approaches()[b].distanceAU;
        return da != db ? da < db : a < b;
    };
    const std::vector<std::uint32_t> closest = neo::dsa::topK(all, 10, closer);
    std::vector<std::uint32_t> sorted = all;
    std::partial_sort(sorted.begin(), sorted.begin() + 10, sorted.end(), closer);
    sorted.resize(10);
    check(closest == sorted, "top-10 closest matches a partial sort of the whole set");
    if (!closest.empty()) {
        const neo::CloseApproach& first = dataset.approaches()[closest[0]];
        const std::string& pdes = dataset.records()[first.objectIndex].object.pdes;
        std::printf("  closest approach: %s at %.6f au\n", pdes.c_str(), first.distanceAU);
    }

    neo::dsa::SizeBucketIndex sizes;
    sizes.build(dataset, neo::dsa::DiameterPolicy::MeasuredOrEstimate);
    check(sizes.checkInvariants(dataset, error), "the real size index is sound", error);
    for (std::size_t b = 0; b < neo::dsa::kSizeBucketCount; ++b) {
        const neo::dsa::SizeBucket bucket = static_cast<neo::dsa::SizeBucket>(b);
        std::printf("  %-14s %6zu objects\n", neo::dsa::toString(bucket), sizes.count(bucket));
    }

    neo::dsa::YearBucketIndex years;
    years.build(dataset);
    check(years.checkInvariants(dataset, error), "the real year index is sound", error);
    std::printf("  years %d..%d, %zu buckets\n", years.firstYear(), years.lastYear(), years.yearCount());
}

} // namespace

int main() {
    testHashMapBasics();
    testHashMapFuzz();
    testSortBasics();
    testSortedView();
    testHeapBasics();
    testHeapFuzz();
    testAvlBasics();
    testAvlFuzz();
    testBuckets();
    testRealDataset();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
