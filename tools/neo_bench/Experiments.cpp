#include "Experiments.h"

#include "SyntheticData.h"
#include "Timing.h"
#include "neo/dsa/AvlTree.h"
#include "neo/dsa/BinaryHeap.h"
#include "neo/dsa/Instrumentation.h"
#include "neo/dsa/HashMap.h"
#include "neo/dsa/IndexedHashMap.h"
#include "neo/dsa/Inline.h"
#include "neo/dsa/Sort.h"
#include "neo/ingest/SbdbParser.h"
#include "neo/model/JulianDate.h"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <utility>

namespace neo {
namespace bench {

namespace {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 6364136223846793005ull + 1442695040888963407ull) {
        for (int i = 0; i < 4; ++i) {
            next();
        }
    }
    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n); }

private:
    std::uint64_t state_;
};

std::string fmt(const char* format, double a) {
    char buf[96];
    std::snprintf(buf, sizeof buf, format, a);
    return buf;
}

std::string fmt2(const char* format, double a, double b) {
    char buf[128];
    std::snprintf(buf, sizeof buf, format, a, b);
    return buf;
}

BenchRow makeRow(const DatasetUnderTest& d, const char* experiment, const std::string& variant, const std::string& param,
                 std::size_t n, std::size_t ops, const Timing& t, const char* unit) {
    BenchRow row;
    row.experiment = experiment;
    row.dataset = d.name;
    row.datasetKind = d.kind;
    row.variant = variant;
    row.param = param;
    row.n = n;
    row.opsPerRepeat = ops;
    row.repeats = t.repeats();
    row.median = t.median();
    row.min = t.min();
    row.max = t.max();
    row.unit = unit;
    return row;
}

BenchRow bytesRow(const DatasetUnderTest& d, const char* experiment, const std::string& variant, std::size_t n,
                  std::size_t bytes, const std::string& note) {
    BenchRow row;
    row.experiment = experiment;
    row.dataset = d.name;
    row.datasetKind = d.kind;
    row.variant = variant;
    row.n = n;
    row.repeats = 1;
    row.median = row.min = row.max = static_cast<double>(bytes);
    row.unit = "bytes";
    row.note = note;
    return row;
}

void requireStable(const DatasetUnderTest& d, const char* experiment, const std::string& variant, const Timing& t,
                   Recorder& out) {
    if (!t.stable) {
        out.fail(std::string(experiment) + " / " + d.name + " / " + variant + ": the workload returned different results on different repeats");
    }
}

bool tooSmall(const DatasetUnderTest& d, const char* experiment, std::size_t have, std::size_t need, Recorder& out) {
    if (have >= need) {
        return false;
    }
    out.notRun(experiment, d.name, d.kind,
               "needs at least " + std::to_string(need) + " rows, the dataset has " + std::to_string(have));
    return true;
}

} // namespace

int effectiveRepeats(const ExperimentConfig& config) { return std::max(5, config.repeats); }

// ---------------------------------------------------------------------------------
// Every timed workload below is a function of its own that the compiler may not inline (NEO_NOINLINE).
// Why: the same top-K source took 1.2 ms when the compiler inlined it into a large benchmark function and
// 0.48 ms as a function of its own, because the inlined loop lost its registers (docs/DSA_NOTES.md). When
// variants are lambdas inside one big function, which of them wins can depend on how the optimiser happened
// to allocate registers in that function, not on the data structure. Behind a call boundary each variant is
// compiled the same way every time.
// ---------------------------------------------------------------------------------

// exact lookup
// ---------------------------------------------------------------------------------

namespace {

struct RecordKey {
    const std::vector<AsteroidRecord>* records;
    const std::string& operator()(std::uint32_t record) const { return (*records)[record].object.pdes; }
};

using IndexedMap = dsa::IndexedHashMap<RecordKey>;
using ClassicMap = dsa::HashMap<std::string, std::uint32_t>;
using StdMap = std::unordered_map<std::string, std::uint32_t>;

NEO_NOINLINE std::uint32_t linearFind(const std::vector<AsteroidRecord>& records, const std::string& key) {
    const std::uint32_t n = static_cast<std::uint32_t>(records.size());
    for (std::uint32_t r = 0; r < n; ++r) {
        if (records[r].object.pdes == key) {
            return r;
        }
    }
    return kInvalidRecord;
}

NEO_NOINLINE std::uint64_t lookupLinear(const std::vector<AsteroidRecord>& records, const std::vector<std::string>& queries, std::size_t count) {
    std::uint64_t acc = kChecksumSeed;
    for (std::size_t i = 0; i < count; ++i) {
        acc = mixChecksum(acc, linearFind(records, queries[i]));
    }
    return acc;
}

NEO_NOINLINE std::uint64_t lookupClassic(const ClassicMap& map, const std::vector<std::string>& queries) {
    std::uint64_t acc = kChecksumSeed;
    for (const std::string& q : queries) {
        const std::uint32_t* v = map.find(q);
        acc = mixChecksum(acc, v == nullptr ? kInvalidRecord : *v);
    }
    return acc;
}

NEO_NOINLINE std::uint64_t lookupStd(const StdMap& map, const std::vector<std::string>& queries) {
    std::uint64_t acc = kChecksumSeed;
    for (const std::string& q : queries) {
        const auto it = map.find(q);
        acc = mixChecksum(acc, it == map.end() ? kInvalidRecord : it->second);
    }
    return acc;
}

NEO_NOINLINE std::uint64_t lookupIndexed(const IndexedMap& map, const std::vector<std::string>& queries) {
    std::uint64_t acc = kChecksumSeed;
    for (const std::string& q : queries) {
        const std::uint32_t v = map.find(q);
        acc = mixChecksum(acc, v == IndexedMap::kNotFound ? kInvalidRecord : v);
    }
    return acc;
}

NEO_NOINLINE ClassicMap buildClassic(const std::vector<AsteroidRecord>& records) {
    ClassicMap fresh(records.size());
    for (std::uint32_t r = 0; r < records.size(); ++r) {
        fresh.insert(records[r].object.pdes, r);
    }
    return fresh;
}

NEO_NOINLINE StdMap buildStd(const std::vector<AsteroidRecord>& records) {
    StdMap fresh;
    fresh.reserve(records.size());
    for (std::uint32_t r = 0; r < records.size(); ++r) {
        fresh.emplace(records[r].object.pdes, r);
    }
    return fresh;
}

NEO_NOINLINE IndexedMap buildIndexed(const std::vector<AsteroidRecord>& records) {
    IndexedMap fresh{RecordKey{&records}, records.size()};
    for (std::uint32_t r = 0; r < records.size(); ++r) {
        fresh.insert(r);
    }
    return fresh;
}

} // namespace

void runLookupExperiment(const DatasetUnderTest& d, const ExperimentConfig& config, Recorder& out) {
    const std::vector<AsteroidRecord>& records = d.dataset->records();
    const std::size_t n = records.size();
    if (tooSmall(d, "lookup", n, 8, out)) {
        return;
    }
    const int reps = effectiveRepeats(config);
    Rng rng(config.seed ^ 0x100C);

    // Three present designations for every absent one (an absent key ends in '~').
    const std::size_t total = config.lookupQueries;
    std::vector<std::string> queries;
    queries.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        std::string key = records[rng.below(static_cast<std::uint32_t>(n))].object.pdes;
        if (i % 4 == 3) {
            key += '~';
        }
        queries.push_back(std::move(key));
    }
    const std::size_t linearQueries = std::max<std::size_t>(1, std::min(total, config.linearBudget / n));

    // --- build (timed) ------------------------------------------------------------
    ClassicMap classic;
    StdMap reference;
    IndexedMap indexed{RecordKey{&records}};
    const Timing buildClassicT = measureMs(reps, [&] {
        classic = buildClassic(records);
        return static_cast<std::uint64_t>(classic.size());
    });
    const Timing buildStdT = measureMs(reps, [&] {
        reference = buildStd(records);
        return static_cast<std::uint64_t>(reference.size());
    });
    const Timing buildIndexedT = measureMs(reps, [&] {
        indexed = buildIndexed(records);
        return static_cast<std::uint64_t>(indexed.size());
    });

    // --- agreement (untimed) --------------------------------------------------------
    std::uint64_t agreedChecksum = kChecksumSeed;
    bool agree = true;
    for (std::size_t i = 0; i < total; ++i) {
        const auto it = reference.find(queries[i]);
        const std::uint32_t truth = it == reference.end() ? kInvalidRecord : it->second;
        const std::uint32_t* c = classic.find(queries[i]);
        const std::uint32_t mine = c == nullptr ? kInvalidRecord : *c;
        const std::uint32_t found = indexed.find(queries[i]);
        const std::uint32_t lean = found == IndexedMap::kNotFound ? kInvalidRecord : found;
        agree = agree && mine == truth && lean == truth;
        if (i < linearQueries) {
            agree = agree && linearFind(records, queries[i]) == truth;
            agreedChecksum = mixChecksum(agreedChecksum, truth);
        }
    }
    if (!agree) {
        out.fail("lookup / " + d.name + ": the lookup variants returned different records for the same designations");
    }

    // --- lookup (timed) ---------------------------------------------------------------
    const std::string param = "queries=" + std::to_string(total) + " (75% present)";
    auto lookupRow = [&](const std::string& variant, std::size_t ops, const Timing& t, const std::string& note) {
        requireStable(d, "lookup", variant, t, out);
        BenchRow row = makeRow(d, "lookup", variant, param, n, ops, t, "ns/op");
        row.result = agreedChecksum;
        row.hasResult = true;
        row.note = note;
        out.add(std::move(row));
    };

    lookupRow("linear scan", linearQueries, measureNsPerOp(reps, linearQueries, [&] { return lookupLinear(records, queries, linearQueries); }),
              std::to_string(linearQueries) + " lookups per repeat (budget " + std::to_string(config.linearBudget) + " element visits)");

    const dsa::ProbeStats classicProbes = classic.probeStats();
    lookupRow("dsa::HashMap<string,uint32>", total, measureNsPerOp(reps, total, [&] { return lookupClassic(classic, queries); }),
              fmt2("mean probe %.2f, worst %.0f", classicProbes.meanProbe, static_cast<double>(classicProbes.maxProbe)));
    lookupRow("std::unordered_map<string,uint32>", total, measureNsPerOp(reps, total, [&] { return lookupStd(reference, queries); }), "");
    const dsa::ProbeStats indexedProbes = indexed.probeStats();
    lookupRow("dsa::IndexedHashMap (record index, tag)", total, measureNsPerOp(reps, total, [&] { return lookupIndexed(indexed, queries); }),
              fmt2("mean probe %.2f, worst %.0f", indexedProbes.meanProbe, static_cast<double>(indexedProbes.maxProbe)));

    // build time
    auto buildRow = [&](const std::string& variant, const Timing& t) {
        requireStable(d, "lookup_build", variant, t, out);
        out.add(makeRow(d, "lookup_build", variant, "", n, 1, t, "ms"));
    };
    buildRow("dsa::HashMap<string,uint32>", buildClassicT);
    buildRow("std::unordered_map<string,uint32>", buildStdT);
    buildRow("dsa::IndexedHashMap (record index, tag)", buildIndexedT);

    // memory
    const std::size_t stdBytes = reference.bucket_count() * sizeof(void*) +
                                 reference.size() * (sizeof(std::pair<const std::string, std::uint32_t>) + sizeof(void*) + 16);
    out.add(bytesRow(d, "lookup_memory", "dsa::HashMap<string,uint32>", n, classic.memoryBytes(),
                     fmt("%.1f bytes per entry", static_cast<double>(classic.memoryBytes()) / static_cast<double>(n))));
    out.add(bytesRow(d, "lookup_memory", "dsa::IndexedHashMap (record index, tag)", n, indexed.memoryBytes(),
                     fmt("%.1f bytes per entry", static_cast<double>(indexed.memoryBytes()) / static_cast<double>(n))));
    out.add(bytesRow(d, "lookup_memory", "std::unordered_map<string,uint32>", n, stdBytes,
                     "ESTIMATE: buckets*8 + entries*(node 48 + ~16 allocator overhead), libstdc++ layout; designations fit the small-string buffer"));
}

// ---------------------------------------------------------------------------------
// range
// ---------------------------------------------------------------------------------

namespace {

struct DateKey {
    const std::vector<double>* jd;
    std::optional<double> operator()(std::uint32_t k) const { return (*jd)[k]; }
};

struct Window {
    double lo, hi;
};

NEO_NOINLINE dsa::SortedView buildDateView(const std::vector<double>& jd) { return dsa::buildSortedView(jd.size(), DateKey{&jd}); }

NEO_NOINLINE dsa::AvlTree<double> buildDateTree(const std::vector<double>& jd) {
    dsa::AvlTree<double> fresh;
    fresh.reserve(jd.size());
    for (std::uint32_t k = 0; k < jd.size(); ++k) {
        fresh.insert(jd[k], k);
    }
    return fresh;
}

// each variant returns, per window, (count, sum of approach indices): order-independent
NEO_NOINLINE std::uint64_t rangeLinear(const std::vector<double>& jd, const std::vector<Window>& windows) {
    const std::uint32_t m = static_cast<std::uint32_t>(jd.size());
    std::uint64_t acc = kChecksumSeed;
    for (const Window& w : windows) {
        std::uint64_t count = 0, sum = 0;
        for (std::uint32_t k = 0; k < m; ++k) {
            if (w.lo <= jd[k] && jd[k] <= w.hi) {
                ++count;
                sum += k;
            }
        }
        acc = mixChecksum(mixChecksum(acc, count), sum);
    }
    return acc;
}

NEO_NOINLINE std::uint64_t rangeSorted(const dsa::SortedView& view, const std::vector<Window>& windows) {
    std::uint64_t acc = kChecksumSeed;
    for (const Window& w : windows) {
        const std::pair<std::size_t, std::size_t> r = view.range(w.lo, w.hi);
        std::uint64_t sum = 0;
        for (std::size_t k = r.first; k < r.second; ++k) {
            sum += view.order[k];
        }
        acc = mixChecksum(mixChecksum(acc, r.second - r.first), sum);
    }
    return acc;
}

NEO_NOINLINE std::uint64_t rangeAvl(const dsa::AvlTree<double>& tree, const std::vector<Window>& windows) {
    std::uint64_t acc = kChecksumSeed;
    for (const Window& w : windows) {
        std::uint64_t sum = 0;
        const std::size_t count = tree.range(w.lo, w.hi, [&sum](double, std::uint32_t k) { sum += k; });
        acc = mixChecksum(mixChecksum(acc, count), sum);
    }
    return acc;
}

std::vector<Window> makeWindows(const std::vector<double>& sortedDates, double selectivity, std::size_t count, Rng& rng) {
    const std::size_t m = sortedDates.size();
    const std::size_t width = std::max<std::size_t>(1, static_cast<std::size_t>(selectivity * static_cast<double>(m) + 0.5));
    std::vector<Window> windows;
    for (std::size_t w = 0; w < count; ++w) {
        const std::size_t start = rng.below(static_cast<std::uint32_t>(m - width + 1));
        windows.push_back({sortedDates[start], sortedDates[start + width - 1]});
    }
    return windows;
}

} // namespace

void runRangeExperiment(const DatasetUnderTest& d, const ExperimentConfig& config, Recorder& out) {
    const std::vector<double>& jd = d.engine->indexes().app.jd;
    const std::size_t m = jd.size();
    if (tooSmall(d, "range", m, 20, out)) {
        return;
    }
    const int reps = effectiveRepeats(config);
    Rng rng(config.seed ^ 0x7A96E);

    dsa::SortedView view;
    dsa::AvlTree<double> tree;
    const Timing buildView = measureMs(reps, [&] {
        view = buildDateView(jd);
        return static_cast<std::uint64_t>(view.size());
    });
    const Timing buildTree = measureMs(reps, [&] {
        tree = buildDateTree(jd);
        return static_cast<std::uint64_t>(tree.size());
    });
    requireStable(d, "range_build", "sorted view", buildView, out);
    requireStable(d, "range_build", "AVL tree", buildTree, out);
    out.add(makeRow(d, "range_build", "sorted view (merge sort + keys)", "", m, 1, buildView, "ms"));
    out.add(makeRow(d, "range_build", "AVL tree", "", m, 1, buildTree, "ms"));
    out.add(bytesRow(d, "range_memory", "sorted view (order + keys)", m, view.memoryBytes(), ""));
    out.add(bytesRow(d, "range_memory", "AVL tree", m, tree.memoryBytes(), ""));
    out.add(bytesRow(d, "range_memory", "dense column scanned by the linear variant", m, jd.capacity() * sizeof(double),
                     "exists anyway (ObjectColumns/ApproachColumns); not an index"));

    std::vector<double> sortedDates = jd;
    std::sort(sortedDates.begin(), sortedDates.end());

    const double selectivities[] = {0.001, 0.01, 0.1};
    for (const double selectivity : selectivities) {
        const std::vector<Window> windows = makeWindows(sortedDates, selectivity, config.rangeWindows, rng);
        const std::size_t ops = windows.size();
        const std::size_t width = std::max<std::size_t>(1, static_cast<std::size_t>(selectivity * static_cast<double>(m) + 0.5));

        const std::uint64_t truth = rangeLinear(jd, windows);
        const std::string param = fmt("selectivity=%.3f", selectivity) + " (" + std::to_string(width) + " of " +
                                  std::to_string(m) + " approaches per window)";
        auto add = [&](const std::string& variant, const Timing& t) {
            requireStable(d, "range", variant, t, out);
            if (t.result != truth) {
                out.fail("range / " + d.name + " / " + variant + " / " + param + ": result differs from the linear scan");
            }
            BenchRow row = makeRow(d, "range", variant, param, m, ops, t, "ns/op");
            row.result = t.result;
            row.hasResult = true;
            row.note = "ns per window; " + std::to_string(ops) + " windows per repeat";
            out.add(std::move(row));
        };
        add("linear scan (dense column)", measureNsPerOp(reps, ops, [&] { return rangeLinear(jd, windows); }));
        add("sorted view + binary search", measureNsPerOp(reps, ops, [&] { return rangeSorted(view, windows); }));
        add("AVL tree range walk", measureNsPerOp(reps, ops, [&] { return rangeAvl(tree, windows); }));
    }
}

// ---------------------------------------------------------------------------------
// mutation: the AVL tree's reason to exist
// ---------------------------------------------------------------------------------

namespace {

// A sorted array of (key, payload), ordered by key then payload: what a SortedView is, made mutable.
struct SortedArray {
    std::vector<double>        keys;
    std::vector<std::uint32_t> payload;
};

struct NewEntry {
    double        key;
    std::uint32_t payload;
};

bool entryLess(const NewEntry& a, const NewEntry& b) { return a.key != b.key ? a.key < b.key : a.payload < b.payload; }

NEO_NOINLINE SortedArray makeSortedArray(const dsa::SortedView& view) {
    SortedArray array;
    array.keys = view.keys;
    array.payload = view.order;
    return array;
}

NEO_NOINLINE std::uint64_t avlInsertBatch(dsa::AvlTree<double>& tree, const std::vector<NewEntry>& batch) {
    std::uint64_t inserted = 0;
    for (const NewEntry& e : batch) {
        inserted += tree.insert(e.key, e.payload) ? 1 : 0;
    }
    return inserted;
}

NEO_NOINLINE std::uint64_t avlEraseBatch(dsa::AvlTree<double>& tree, const std::vector<NewEntry>& batch) {
    std::uint64_t erased = 0;
    for (const NewEntry& e : batch) {
        erased += tree.erase(e.key, e.payload) ? 1 : 0;
    }
    return erased;
}

// New payloads are larger than every existing one, so the position of a new entry is after all equal keys.
NEO_NOINLINE std::uint64_t arrayInsertBatch(SortedArray& array, const std::vector<NewEntry>& batch) {
    std::uint64_t inserted = 0;
    for (const NewEntry& e : batch) {
        const std::size_t at = static_cast<std::size_t>(std::upper_bound(array.keys.begin(), array.keys.end(), e.key) - array.keys.begin());
        array.keys.insert(array.keys.begin() + static_cast<std::ptrdiff_t>(at), e.key);
        array.payload.insert(array.payload.begin() + static_cast<std::ptrdiff_t>(at), e.payload);
        ++inserted;
    }
    return inserted;
}

NEO_NOINLINE std::uint64_t arrayEraseBatch(SortedArray& array, const std::vector<NewEntry>& batch) {
    std::uint64_t erased = 0;
    for (const NewEntry& e : batch) {
        std::size_t at = static_cast<std::size_t>(std::lower_bound(array.keys.begin(), array.keys.end(), e.key) - array.keys.begin());
        while (at < array.keys.size() && array.keys[at] == e.key && array.payload[at] != e.payload) {
            ++at;
        }
        if (at < array.keys.size() && array.keys[at] == e.key) {
            array.keys.erase(array.keys.begin() + static_cast<std::ptrdiff_t>(at));
            array.payload.erase(array.payload.begin() + static_cast<std::ptrdiff_t>(at));
            ++erased;
        }
    }
    return erased;
}

// Sort the batch, then merge it into the array in one pass: O(n + B). Returns a checksum of the result.
NEO_NOINLINE std::uint64_t arrayMergeBatch(const SortedArray& array, const std::vector<NewEntry>& batch, SortedArray& merged) {
    std::vector<NewEntry> sorted = batch;
    std::sort(sorted.begin(), sorted.end(), entryLess);
    const std::size_t n = array.keys.size();
    merged.keys.resize(n + sorted.size());
    merged.payload.resize(n + sorted.size());
    std::size_t i = 0, j = 0, o = 0;
    while (i < n && j < sorted.size()) {
        const bool takeNew = sorted[j].key < array.keys[i] || (sorted[j].key == array.keys[i] && sorted[j].payload < array.payload[i]);
        if (takeNew) {
            merged.keys[o] = sorted[j].key;
            merged.payload[o++] = sorted[j++].payload;
        } else {
            merged.keys[o] = array.keys[i];
            merged.payload[o++] = array.payload[i++];
        }
    }
    for (; i < n; ++i, ++o) {
        merged.keys[o] = array.keys[i];
        merged.payload[o] = array.payload[i];
    }
    for (; j < sorted.size(); ++j, ++o) {
        merged.keys[o] = sorted[j].key;
        merged.payload[o] = sorted[j].payload;
    }
    return mixChecksum(mixChecksum(kChecksumSeed, merged.keys.size()), merged.payload[merged.payload.size() / 2]);
}

// Append the batch to the column and rebuild the whole sorted view: the project's own merge sort, O(n log n).
NEO_NOINLINE std::uint64_t arrayRebuildBatch(const std::vector<double>& jd, const std::vector<NewEntry>& batch) {
    std::vector<double> column = jd;
    column.reserve(jd.size() + batch.size());
    for (const NewEntry& e : batch) {
        column.push_back(e.key);
    }
    const dsa::SortedView view = buildDateView(column);
    return mixChecksum(mixChecksum(kChecksumSeed, view.size()), view.order[view.order.size() / 2]);
}

// Two timed phases per repeat (apply, then undo), so the structure returns to what it was and every repeat
// does the same work.
template <class Apply, class Undo>
void timeTwoPhase(int repeats, std::size_t ops, Apply&& apply, Undo&& undo, Timing& first, Timing& second) {
    first = Timing();
    second = Timing();
    const std::uint64_t expectedA = apply();
    const std::uint64_t expectedB = undo();
    first.result = expectedA;
    second.result = expectedB;
    const double perOp = ops == 0 ? 1.0 : static_cast<double>(ops);
    for (int r = 0; r < repeats; ++r) {
        const Clock::time_point t0 = Clock::now();
        const std::uint64_t a = apply();
        const Clock::time_point t1 = Clock::now();
        const std::uint64_t b = undo();
        const Clock::time_point t2 = Clock::now();
        first.stable = first.stable && a == expectedA;
        second.stable = second.stable && b == expectedB;
        first.samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / perOp);
        second.samples.push_back(std::chrono::duration<double, std::nano>(t2 - t1).count() / perOp);
    }
}

} // namespace

void runMutationExperiment(const DatasetUnderTest& d, const ExperimentConfig& config, Recorder& out) {
    const std::vector<double>& jd = d.engine->indexes().app.jd;
    const std::size_t m = jd.size();
    if (tooSmall(d, "mutation", m, 200, out)) {
        return;
    }
    const int reps = effectiveRepeats(config);
    Rng rng(config.seed ^ 0x3A71);

    const dsa::SortedView view = buildDateView(jd);
    dsa::AvlTree<double> tree = buildDateTree(jd);
    SortedArray array = makeSortedArray(view);

    // The mutations: random new keys inside the existing date range, with payloads above every existing one.
    const double lo = *std::min_element(jd.begin(), jd.end());
    const double hi = *std::max_element(jd.begin(), jd.end());
    // A shifting insert costs O(n); keep the batch small enough at large n that a repeat stays well under a second.
    const std::size_t batchSize = m <= 100000 ? std::min<std::size_t>(1000, std::max<std::size_t>(20, m / 10)) : 200;
    auto makeBatch = [&](std::size_t count) {
        std::vector<NewEntry> batch;
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double unit = static_cast<double>(rng.next() >> 11) * (1.0 / 9007199254740992.0);
            batch.push_back({lo + (hi - lo) * unit, static_cast<std::uint32_t>(m + i)});
        }
        return batch;
    };
    const std::vector<NewEntry> batch = makeBatch(batchSize);

    // --- agreement (untimed): both structures, after the same inserts and then the same erases ---------
    {
        avlInsertBatch(tree, batch);
        arrayInsertBatch(array, batch);
        std::vector<std::uint32_t> fromTree;
        fromTree.reserve(tree.size());
        tree.inOrder([&fromTree](double, std::uint32_t k) { fromTree.push_back(k); });
        bool same = fromTree == array.payload && tree.size() == m + batchSize;
        avlEraseBatch(tree, batch);
        arrayEraseBatch(array, batch);
        fromTree.clear();
        tree.inOrder([&fromTree](double, std::uint32_t k) { fromTree.push_back(k); });
        same = same && fromTree == array.payload && tree.size() == m && array.keys.size() == m;
        // and the merge gives the same sequence as inserting one by one
        SortedArray merged;
        arrayMergeBatch(array, batch, merged);
        SortedArray oneByOne = array;
        arrayInsertBatch(oneByOne, batch);
        same = same && merged.keys == oneByOne.keys && merged.payload == oneByOne.payload;
        if (!same) {
            out.fail("mutation / " + d.name + ": the AVL tree, the shifted array and the merged array disagree after the same mutations");
        }
    }

    // --- per-mutation cost -------------------------------------------------------------------------
    Timing avlInsert, avlErase, arrayInsert, arrayErase;
    timeTwoPhase(reps, batchSize, [&] { return avlInsertBatch(tree, batch); }, [&] { return avlEraseBatch(tree, batch); }, avlInsert, avlErase);
    timeTwoPhase(reps, batchSize, [&] { return arrayInsertBatch(array, batch); }, [&] { return arrayEraseBatch(array, batch); }, arrayInsert, arrayErase);
    const std::string param = "n=" + std::to_string(m) + ", batch of " + std::to_string(batchSize);
    auto add = [&](const std::string& variant, const Timing& t, std::size_t ops, const std::string& note) {
        requireStable(d, "mutation", variant, t, out);
        BenchRow row = makeRow(d, "mutation", variant, param, m, ops, t, "ns/op");
        row.result = t.result;
        row.hasResult = true;
        row.note = note;
        out.add(std::move(row));
    };
    add("AVL tree: insert", avlInsert, batchSize, "O(log n) rotations; the structure the stage 5 design is for");
    add("AVL tree: erase", avlErase, batchSize, "O(log n)");
    add("sorted array: insert one (shift)", arrayInsert, batchSize, "binary search, then two memmoves of ~n/2 elements each: O(n)");
    add("sorted array: erase one (shift)", arrayErase, batchSize, "O(n)");

    // --- batch maintenance of the sorted array -----------------------------------------------------
    Timing rebuild;
    {
        const std::vector<NewEntry> small = makeBatch(1000);
        rebuild = measureNsPerOp(reps, 1000, [&] { return arrayRebuildBatch(jd, small); });
        add("sorted array: rebuild everything after 1000 inserts", rebuild, 1000,
            "ns per inserted element = the whole O(n log n) rebuild / 1000; independent of the batch size, so it is " +
                fmt("%.1f", rebuild.median() * 1000.0 / 1.0e6) + " ms per rebuild");
    }
    for (const std::size_t b : {std::size_t(1), std::size_t(10), std::size_t(100), std::size_t(1000), std::size_t(10000)}) {
        const std::vector<NewEntry> mergeBatch = makeBatch(b);
        SortedArray merged;
        const Timing t = measureNsPerOp(reps, b, [&] { return arrayMergeBatch(array, mergeBatch, merged); });
        add("sorted array: merge a sorted batch of B (O(n + B))", t, b,
            "B=" + std::to_string(b) + ": " + fmt("%.3f", t.median() * static_cast<double>(b) / 1.0e6) + " ms per batch");
    }

    // --- queries on the same structures (1 % windows) ----------------------------------------------
    std::vector<double> sortedDates = jd;
    std::sort(sortedDates.begin(), sortedDates.end());
    const std::vector<Window> windows = makeWindows(sortedDates, 0.01, config.rangeWindows, rng);
    const Timing queryAvl = measureNsPerOp(reps, windows.size(), [&] { return rangeAvl(tree, windows); });
    const Timing queryArray = measureNsPerOp(reps, windows.size(), [&] { return rangeSorted(view, windows); });
    if (queryAvl.result != queryArray.result) {
        out.fail("mutation / " + d.name + ": the AVL tree and the sorted view answer a 1% range query differently");
    }
    add("range query, 1% window: AVL tree", queryAvl, windows.size(), "same windows as below; the query half of the trade");
    add("range query, 1% window: sorted array", queryArray, windows.size(), "");

    // --- break-even: range queries per mutation at which the two cost the same ---------------------
    // Per mutation (an insert or an erase, averaged): AVL costs a + x*qa, the shifted array b + x*qs,
    // for x queries; equal at x = (b - a) / (qa - qs). Below x the AVL tree is cheaper overall.
    const double avlMutation = 0.5 * (avlInsert.median() + avlErase.median());
    // A shifting erase can cost several times a shifting insert for a reason that has nothing to do with the
    // data structure: the C library's memmove is much slower for an overlapping forward copy by one element
    // (erase) than a backward one (insert); measured on glibc with plain memmove, 650 us against 65 us for
    // 250,000 doubles. So the array's CHEAPER operation is the fair figure for the main comparison (it favours
    // the array), and the average is reported beside it.
    const double arrayCheaper = std::min(arrayInsert.median(), arrayErase.median());
    const double arrayAverage = 0.5 * (arrayInsert.median() + arrayErase.median());
    const double mergePerElement = [&] {
        // the merge at B = 1000, per element, as the array's cost of a mutation when they arrive in batches
        const std::vector<NewEntry> mergeBatch = makeBatch(1000);
        SortedArray merged;
        return measureNsPerOp(reps, 1000, [&] { return arrayMergeBatch(array, mergeBatch, merged); }).median();
    }();
    auto breakeven = [&](const char* label, double arrayCost) {
        BenchRow row;
        row.experiment = "mutation_breakeven";
        row.dataset = d.name;
        row.datasetKind = d.kind;
        row.variant = label;
        row.param = param;
        row.n = m;
        row.repeats = 1;
        row.unit = "range queries per mutation";
        const double dq = queryAvl.median() - queryArray.median();
        if (arrayCost <= avlMutation) {
            row.median = row.min = row.max = 0.0;
            row.note = "the sorted array's mutation (" + fmt("%.0f", arrayCost) + " ns) is not dearer than the AVL tree's (" +
                       fmt("%.0f", avlMutation) + " ns) at this size: the array wins at any query rate";
        } else if (dq <= 0.0) {
            row.median = row.min = row.max = 1.0e18;
            row.note = "the AVL tree is cheaper on both mutations and queries here: it wins at any rate";
        } else {
            row.median = row.min = row.max = (arrayCost - avlMutation) / dq;
            row.note = "AVL cheaper overall below this many 1% range queries per mutation; mutation " + fmt("%.0f", avlMutation) +
                       " ns (AVL) against " + fmt("%.0f", arrayCost) + " ns (array), query " + fmt("%.0f", queryAvl.median()) + " ns against " +
                       fmt("%.0f", queryArray.median()) + " ns";
        }
        out.add(std::move(row));
    };
    breakeven("AVL tree vs a sorted array shifted on every mutation (its cheaper operation)", arrayCheaper);
    breakeven("AVL tree vs a sorted array shifted on every mutation (average of insert and erase)", arrayAverage);
    breakeven("AVL tree vs a sorted array merged in batches of 1000", mergePerElement);
}

// ---------------------------------------------------------------------------------
// top-K
// ---------------------------------------------------------------------------------

namespace {

std::uint64_t digestK(const std::vector<std::uint32_t>& ids, std::size_t k) {
    std::uint64_t acc = kChecksumSeed;
    for (std::size_t i = 0; i < k && i < ids.size(); ++i) {
        acc = mixChecksum(acc, ids[i]);
    }
    return acc;
}

template <class Better>
NEO_NOINLINE std::uint64_t topkStdSort(const std::vector<std::uint32_t>& all, std::size_t k, const Better& better) {
    std::vector<std::uint32_t> copy = all;
    std::sort(copy.begin(), copy.end(), better);
    return digestK(copy, k);
}

template <class Better>
NEO_NOINLINE std::uint64_t topkMergeSort(const std::vector<std::uint32_t>& all, std::size_t k, const Better& better) {
    std::vector<std::uint32_t> copy = all;
    dsa::mergeSort(copy, better);
    return digestK(copy, k);
}

template <class Better>
NEO_NOINLINE std::uint64_t topkPartialSort(const std::vector<std::uint32_t>& all, std::size_t k, const Better& better) {
    std::vector<std::uint32_t> copy = all;
    std::partial_sort(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(k), copy.end(), better);
    return digestK(copy, k);
}

template <class Better>
NEO_NOINLINE std::uint64_t topkHeap(const std::vector<std::uint32_t>& all, std::size_t k, const Better& better) {
    return digestK(dsa::topK(all, k, better), k);
}

template <class Better>
NEO_NOINLINE std::uint64_t topkPriorityQueue(const std::vector<std::uint32_t>& all, std::size_t k, const Better& better) {
    std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, Better> queue(better);
    for (const std::uint32_t id : all) {
        if (queue.size() < k) {
            queue.push(id);
        } else if (better(id, queue.top())) {
            queue.pop();
            queue.push(id);
        }
    }
    std::vector<std::uint32_t> best(queue.size());
    for (std::size_t i = best.size(); i > 0; --i) {
        best[i - 1] = queue.top();
        queue.pop();
    }
    return digestK(best, k);
}

} // namespace

void runTopKExperiment(const DatasetUnderTest& d, const ExperimentConfig& config, Recorder& out) {
    const std::vector<double>& dist = d.engine->indexes().app.dist;
    const std::size_t m = dist.size();
    if (tooSmall(d, "topk", m, 20, out)) {
        return;
    }
    const int reps = effectiveRepeats(config);

    std::vector<std::uint32_t> all(m);
    std::iota(all.begin(), all.end(), 0u);
    // smaller distance ranks higher; the index breaks ties, so every variant has one right answer
    const auto better = [&dist](std::uint32_t a, std::uint32_t b) {
        if (dist[a] != dist[b]) {
            return dist[a] < dist[b];
        }
        return a < b;
    };

    for (const std::size_t wanted : {std::size_t(10), std::size_t(100), std::size_t(1000)}) {
        const std::size_t k = std::min(wanted, m);
        if (k != wanted) {
            out.notRun("topk", d.name, d.kind, "k=" + std::to_string(wanted) + " exceeds the " + std::to_string(m) + " approaches");
            continue;
        }
        const std::string param = "k=" + std::to_string(k);
        const std::uint64_t truth = topkStdSort(all, k, better);
        auto add = [&](const std::string& variant, const Timing& t, const std::string& note) {
            requireStable(d, "topk", variant, t, out);
            if (t.result != truth) {
                out.fail("topk / " + d.name + " / " + variant + " / " + param + ": result differs from std::sort");
            }
            BenchRow row = makeRow(d, "topk", variant, param, m, 1, t, "ms");
            row.result = t.result;
            row.hasResult = true;
            row.note = note;
            out.add(std::move(row));
        };
        add("std::sort (full), take k", measureMs(reps, [&] { return topkStdSort(all, k, better); }), "copies the " + std::to_string(m) + " indices first");
        add("dsa::mergeSort (full), take k", measureMs(reps, [&] { return topkMergeSort(all, k, better); }), "copies the indices first");
        add("std::partial_sort", measureMs(reps, [&] { return topkPartialSort(all, k, better); }), "copies the indices first");
        add("dsa::topK (heap of k)", measureMs(reps, [&] { return topkHeap(all, k, better); }), "no copy of the candidates");
        add("std::priority_queue (heap of k)", measureMs(reps, [&] { return topkPriorityQueue(all, k, better); }), "no copy of the candidates");
    }
}

// ---------------------------------------------------------------------------------
// combined queries
// ---------------------------------------------------------------------------------

namespace {

double isoJd(const char* iso) {
    double jd = 0.0;
    julianDateFromIsoDate(iso, jd);
    return jd;
}

struct NamedQuery {
    std::string label;
    Query       query;
};

std::vector<NamedQuery> representativeQueries(const Dataset& dataset) {
    std::vector<NamedQuery> list;
    {
        Query q;
        q.pha = TriState::Yes;
        q.dateJd = Range::between(isoJd("2030-01-01"), isoJd("2040-01-01"));
        q.distanceAU = Range::atMost(0.05);
        q.sortBy = SortField::Distance;
        q.topK = 10;
        list.push_back({"PHA, 2030s, 10 closest", q});
    }
    {
        Query q;
        q.diameterKm = Range::atLeast(0.14);
        q.dateJd = Range::between(isoJd("2025-01-01"), isoJd("2035-01-01"));
        list.push_back({"diameter >= 140 m, 2025-2035, all", q});
    }
    {
        Query q;
        q.absoluteMagnitude = Range::atMost(18.0);
        q.moidAU = Range::atMost(0.05);
        q.sortBy = SortField::Moid;
        q.topK = 50;
        list.push_back({"H <= 18 and MOID <= 0.05 au, top 50 by MOID", q});
    }
    {
        Query q;
        const std::size_t mid = dataset.objectCount() / 2;
        q.designation = dataset.records()[mid].object.pdes;
        list.push_back({"exact designation", q});
    }
    {
        Query q;
        q.namePrefix = "2020 ";
        list.push_back({"designation prefix '2020 ', all", q});
    }
    {
        Query q;
        q.orbitClasses = {"APO"};
        q.inclinationDeg = Range::atLeast(20.0);
        q.eccentricity = Range::atLeast(0.5);
        q.sortBy = SortField::SemiMajorAxis;
        q.direction = SortDirection::Descending;
        q.topK = 100;
        list.push_back({"APO, i >= 20, e >= 0.5, top 100 by a (desc)", q});
    }
    {
        Query q;
        q.velocityKms = Range::atLeast(25.0);
        q.distanceAU = Range::atMost(0.01);
        q.sortBy = SortField::Velocity;
        q.direction = SortDirection::Descending;
        q.topK = 20;
        list.push_back({"v >= 25 km/s and dist <= 0.01 au, top 20 by v", q});
    }
    {
        Query q;
        q.dateJd = Range::between(isoJd("2028-01-01"), isoJd("2028-12-31"));
        q.grazing = TriState::No;
        q.sortBy = SortField::Distance;
        list.push_back({"one year (2028), not grazing, by distance", q});
    }
    return list;
}

std::uint64_t digestResult(const QueryResult& r) {
    std::uint64_t acc = mixChecksum(kChecksumSeed, r.totalObjects);
    acc = mixChecksum(acc, r.totalApproaches);
    const std::size_t limit = std::min<std::size_t>(r.rows.size(), 1000);
    for (std::size_t i = 0; i < limit; ++i) {
        acc = mixChecksum(acc, r.rows[i].object);
        acc = mixChecksum(acc, r.rows[i].approachCount);
    }
    return acc;
}

} // namespace

void runQueryExperiment(const DatasetUnderTest& d, const ExperimentConfig& config, Recorder& out) {
    const std::size_t n = d.dataset->objectCount();
    if (tooSmall(d, "query", n, 8, out)) {
        return;
    }
    const int reps = effectiveRepeats(config);
    const ExecMode modes[] = {ExecMode::Naive, ExecMode::FixedOrder, ExecMode::Planned};

    for (const NamedQuery& named : representativeQueries(*d.dataset)) {
        QueryResult results[3];
        for (int i = 0; i < 3; ++i) {
            results[i] = d.engine->run(named.query, modes[i]);
            if (!results[i].ok) {
                out.fail("query / " + d.name + " / " + named.label + ": rejected by validation");
            }
        }
        const std::uint64_t truth = digestResult(results[0]);
        for (int i = 1; i < 3; ++i) {
            if (digestResult(results[i]) != truth) {
                out.fail("query / " + d.name + " / " + named.label + ": " + toString(modes[i]) +
                         " returned a different result from the naive scan");
            }
        }
        for (int i = 0; i < 3; ++i) {
            const Timing t = measureMs(reps, [&] { return digestResult(d.engine->run(named.query, modes[i])); });
            requireStable(d, "query", toString(modes[i]), t, out);
            BenchRow row = makeRow(d, "query", toString(modes[i]), named.label, n, 1, t, "ms");
            row.result = truth;
            row.hasResult = true;
            const QueryStats& s = results[i].stats;
            row.note = "matched " + std::to_string(s.matchedObjects) + " objects; candidates " +
                       std::to_string(s.actualCandidates) + "; predicate evaluations " +
                       std::to_string(s.predicateEvaluations) + "; driver: " + s.driverText;
            out.add(std::move(row));
        }
    }
}

// ---------------------------------------------------------------------------------
// index build and memory
// ---------------------------------------------------------------------------------

namespace {

std::size_t stringHeap(const std::string& s) { return s.capacity() > 15 ? s.capacity() + 1 : 0; }

} // namespace

void runMemoryExperiments(const DatasetUnderTest& d, const ExperimentConfig& config, Recorder& out) {
    const std::size_t n = d.dataset->objectCount();
    if (tooSmall(d, "index_build", n, 1, out)) {
        return;
    }
    const int reps = effectiveRepeats(config);

    std::vector<std::vector<double>> buildMs;
    std::vector<double> totalMs;
    std::vector<IndexBuildInfo> info;
    for (int r = 0; r < reps; ++r) {
        QueryEngine engine(*d.dataset);
        engine.build();
        const std::vector<IndexBuildInfo>& report = engine.buildReport();
        if (info.empty()) {
            info = report;
            buildMs.resize(report.size());
        }
        for (std::size_t i = 0; i < report.size(); ++i) {
            buildMs[i].push_back(report[i].buildMs);
        }
        totalMs.push_back(engine.totalBuildMs());
    }
    auto buildRow = [&](const std::string& name, const std::vector<double>& samples, std::size_t entries) {
        Timing t;
        t.samples = samples;
        BenchRow row = makeRow(d, "index_build", name, "", entries, 1, t, "ms");
        row.note = "median of " + std::to_string(reps) + " full QueryEngine::build() runs";
        out.add(std::move(row));
    };
    std::size_t totalBytes = 0;
    for (std::size_t i = 0; i < info.size(); ++i) {
        buildRow(info[i].name, buildMs[i], info[i].entries);
        out.add(bytesRow(d, "index_memory", info[i].name, info[i].entries, info[i].bytes, ""));
        totalBytes += info[i].bytes;
    }
    buildRow("TOTAL (all indexes)", totalMs, n);
    out.add(bytesRow(d, "index_memory", "TOTAL (all indexes)", n, totalBytes,
                     fmt("%.1f bytes per object", static_cast<double>(totalBytes) / static_cast<double>(n))));

    // master vectors
    const std::vector<AsteroidRecord>& records = d.dataset->records();
    std::size_t heap = 0;
    for (const AsteroidRecord& r : records) {
        const Asteroid& a = r.object;
        heap += stringHeap(a.pdes) + stringHeap(a.spkid) + stringHeap(a.name) + stringHeap(a.fullName) +
                stringHeap(a.orbital.orbitId) + stringHeap(a.classification.orbitClass);
    }
    out.add(bytesRow(d, "master_memory", "records vector (sizeof(AsteroidRecord) * objects)", n,
                     records.size() * sizeof(AsteroidRecord),
                     "sizeof(AsteroidRecord) = " + std::to_string(sizeof(AsteroidRecord))));
    out.add(bytesRow(d, "master_memory", "record string heap (ESTIMATE)", n, heap,
                     "strings longer than the 15-character small-string buffer, by capacity()+1"));
    out.add(bytesRow(d, "master_memory", "approaches vector (sizeof(CloseApproach) * approaches)",
                     d.dataset->approachCount(), d.dataset->approachCount() * sizeof(CloseApproach),
                     "sizeof(CloseApproach) = " + std::to_string(sizeof(CloseApproach))));
}

// ---------------------------------------------------------------------------------
// ingestion by page size
// ---------------------------------------------------------------------------------

void runIngestExperiment(const ExperimentConfig& config, std::size_t totalObjects,
                         const std::vector<std::size_t>& pageSizes, Recorder& out) {
    const int reps = effectiveRepeats(config);
    SyntheticOptions options;
    options.objects = totalObjects;
    options.seed = config.seed ^ 0x1465;
    SyntheticInfo info;
    const Dataset source = makeSyntheticDataset(options, &info);

    DatasetUnderTest d;
    d.name = "synthetic-" + std::to_string(totalObjects);
    d.kind = info.kind;

    for (const std::size_t pageSize : pageSizes) {
        std::vector<std::string> pages;
        std::size_t bytes = 0;
        for (std::size_t first = 0; first < totalObjects; first += pageSize) {
            pages.push_back(makeSbdbPageJson(source, first, pageSize));
            bytes += pages.back().size();
        }
        std::size_t accepted = 0;
        std::size_t rejected = 0;
        bool parsedAll = true;
        const Timing t = measureNsPerOp(reps, totalObjects, [&] {
            std::vector<Asteroid> objects;
            objects.reserve(totalObjects);
            accepted = 0;
            rejected = 0;
            for (const std::string& page : pages) {
                ValidationReport report;
                const ParseStatus status = parseSbdbObjects(page, objects, report);
                parsedAll = parsedAll && status.ok;
                accepted += report.rowsAccepted;
                rejected += report.rowsRejected;
            }
            return static_cast<std::uint64_t>(objects.size());
        });
        if (!parsedAll || accepted != totalObjects || rejected != 0) {
            out.fail("ingest / page=" + std::to_string(pageSize) + ": the parser accepted " + std::to_string(accepted) +
                     " of " + std::to_string(totalObjects) + " generated objects (" + std::to_string(rejected) + " rejected)");
        }
        requireStable(d, "ingest", "parseSbdbObjects", t, out);
        BenchRow row = makeRow(d, "ingest", "parseSbdbObjects", "page=" + std::to_string(pageSize), totalObjects,
                               totalObjects, t, "ns/object");
        row.result = accepted;
        row.hasResult = true;
        row.note = std::to_string(pages.size()) + " pages, " + fmt("%.2f", static_cast<double>(bytes) / 1048576.0) +
                   " MB of JSON; PARSE ONLY: no network, no cache, no politeness delay (the real run is dominated by those)";
        out.add(std::move(row));
    }
}

} // namespace bench
} // namespace neo
