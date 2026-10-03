#include "Experiments.h"

#include "SyntheticData.h"
#include "Timing.h"
#include "neo/dsa/AvlTree.h"
#include "neo/dsa/BinaryHeap.h"
#include "neo/dsa/HashMap.h"
#include "neo/dsa/IndexedHashMap.h"
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

    const Timing buildClassic = measureMs(reps, [&] {
        ClassicMap fresh(n);
        for (std::uint32_t r = 0; r < n; ++r) {
            fresh.insert(records[r].object.pdes, r);
        }
        classic = std::move(fresh);
        return static_cast<std::uint64_t>(classic.size());
    });
    const Timing buildStd = measureMs(reps, [&] {
        StdMap fresh;
        fresh.reserve(n);
        for (std::uint32_t r = 0; r < n; ++r) {
            fresh.emplace(records[r].object.pdes, r);
        }
        reference = std::move(fresh);
        return static_cast<std::uint64_t>(reference.size());
    });
    const Timing buildIndexed = measureMs(reps, [&] {
        IndexedMap fresh{RecordKey{&records}, n};
        for (std::uint32_t r = 0; r < n; ++r) {
            fresh.insert(r);
        }
        indexed = std::move(fresh);
        return static_cast<std::uint64_t>(indexed.size());
    });

    // --- agreement (untimed) --------------------------------------------------------
    auto linearFind = [&](const std::string& key) -> std::uint32_t {
        for (std::uint32_t r = 0; r < n; ++r) {
            if (records[r].object.pdes == key) {
                return r;
            }
        }
        return kInvalidRecord;
    };
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
            agree = agree && linearFind(queries[i]) == truth;
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

    const Timing tLinear = measureNsPerOp(reps, linearQueries, [&] {
        std::uint64_t acc = kChecksumSeed;
        for (std::size_t i = 0; i < linearQueries; ++i) {
            acc = mixChecksum(acc, linearFind(queries[i]));
        }
        return acc;
    });
    lookupRow("linear scan", linearQueries, tLinear,
              std::to_string(linearQueries) + " lookups per repeat (budget " + std::to_string(config.linearBudget) +
                  " element visits)");

    const Timing tClassic = measureNsPerOp(reps, total, [&] {
        std::uint64_t acc = kChecksumSeed;
        for (const std::string& q : queries) {
            const std::uint32_t* v = classic.find(q);
            acc = mixChecksum(acc, v == nullptr ? kInvalidRecord : *v);
        }
        return acc;
    });
    const dsa::ProbeStats classicProbes = classic.probeStats();
    lookupRow("dsa::HashMap<string,uint32>", total, tClassic,
              fmt2("mean probe %.2f, worst %.0f", classicProbes.meanProbe, static_cast<double>(classicProbes.maxProbe)));

    const Timing tStd = measureNsPerOp(reps, total, [&] {
        std::uint64_t acc = kChecksumSeed;
        for (const std::string& q : queries) {
            const auto it = reference.find(q);
            acc = mixChecksum(acc, it == reference.end() ? kInvalidRecord : it->second);
        }
        return acc;
    });
    lookupRow("std::unordered_map<string,uint32>", total, tStd, "");

    const Timing tIndexed = measureNsPerOp(reps, total, [&] {
        std::uint64_t acc = kChecksumSeed;
        for (const std::string& q : queries) {
            const std::uint32_t v = indexed.find(q);
            acc = mixChecksum(acc, v == IndexedMap::kNotFound ? kInvalidRecord : v);
        }
        return acc;
    });
    const dsa::ProbeStats indexedProbes = indexed.probeStats();
    lookupRow("dsa::IndexedHashMap (record index, tag)", total, tIndexed,
              fmt2("mean probe %.2f, worst %.0f", indexedProbes.meanProbe, static_cast<double>(indexedProbes.maxProbe)));

    // build time
    auto buildRow = [&](const std::string& variant, const Timing& t) {
        requireStable(d, "lookup_build", variant, t, out);
        BenchRow row = makeRow(d, "lookup_build", variant, "", n, 1, t, "ms");
        out.add(std::move(row));
    };
    buildRow("dsa::HashMap<string,uint32>", buildClassic);
    buildRow("std::unordered_map<string,uint32>", buildStd);
    buildRow("dsa::IndexedHashMap (record index, tag)", buildIndexed);

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
        view = dsa::buildSortedView(m, DateKey{&jd});
        return static_cast<std::uint64_t>(view.size());
    });
    const Timing buildTree = measureMs(reps, [&] {
        dsa::AvlTree<double> fresh;
        fresh.reserve(m);
        for (std::uint32_t k = 0; k < m; ++k) {
            fresh.insert(jd[k], k);
        }
        tree = std::move(fresh);
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

    struct Window {
        double lo, hi;
    };
    const double selectivities[] = {0.001, 0.01, 0.1};
    for (const double selectivity : selectivities) {
        const std::size_t width = std::max<std::size_t>(1, static_cast<std::size_t>(selectivity * static_cast<double>(m) + 0.5));
        std::vector<Window> windows;
        for (std::size_t w = 0; w < config.rangeWindows; ++w) {
            const std::size_t start = rng.below(static_cast<std::uint32_t>(m - width + 1));
            windows.push_back({sortedDates[start], sortedDates[start + width - 1]});
        }
        const std::size_t ops = windows.size();

        // each variant returns, per window, (count, sum of approach indices): order-independent
        auto linear = [&] {
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
        };
        auto sorted = [&] {
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
        };
        auto avl = [&] {
            std::uint64_t acc = kChecksumSeed;
            for (const Window& w : windows) {
                std::uint64_t sum = 0;
                const std::size_t count = tree.range(w.lo, w.hi, [&sum](double, std::uint32_t k) { sum += k; });
                acc = mixChecksum(mixChecksum(acc, count), sum);
            }
            return acc;
        };

        const std::uint64_t truth = linear();
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
        add("linear scan (dense column)", measureNsPerOp(reps, ops, linear));
        add("sorted view + binary search", measureNsPerOp(reps, ops, sorted));
        add("AVL tree range walk", measureNsPerOp(reps, ops, avl));
    }
}

// ---------------------------------------------------------------------------------
// top-K
// ---------------------------------------------------------------------------------

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
    const auto digest = [](const std::vector<std::uint32_t>& ids, std::size_t k) {
        std::uint64_t acc = kChecksumSeed;
        for (std::size_t i = 0; i < k && i < ids.size(); ++i) {
            acc = mixChecksum(acc, ids[i]);
        }
        return acc;
    };

    for (const std::size_t wanted : {std::size_t(10), std::size_t(100), std::size_t(1000)}) {
        const std::size_t k = std::min(wanted, m);
        if (k != wanted) {
            out.notRun("topk", d.name, d.kind, "k=" + std::to_string(wanted) + " exceeds the " + std::to_string(m) + " approaches");
            continue;
        }
        const std::string param = "k=" + std::to_string(k);

        auto stdSort = [&] {
            std::vector<std::uint32_t> copy = all;
            std::sort(copy.begin(), copy.end(), better);
            return digest(copy, k);
        };
        auto mergeSorted = [&] {
            std::vector<std::uint32_t> copy = all;
            dsa::mergeSort(copy, better);
            return digest(copy, k);
        };
        auto partial = [&] {
            std::vector<std::uint32_t> copy = all;
            std::partial_sort(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(k), copy.end(), better);
            return digest(copy, k);
        };
        auto heap = [&] { return digest(dsa::topK(all, k, better), k); };
        auto pq = [&] {
            std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, decltype(better)> queue(better);
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
            return digest(best, k);
        };

        const std::uint64_t truth = stdSort();
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
        add("std::sort (full), take k", measureMs(reps, stdSort), "copies the " + std::to_string(m) + " indices first");
        add("dsa::mergeSort (full), take k", measureMs(reps, mergeSorted), "copies the indices first");
        add("std::partial_sort", measureMs(reps, partial), "copies the indices first");
        add("dsa::topK (heap of k)", measureMs(reps, heap), "no copy of the candidates");
        add("std::priority_queue (heap of k)", measureMs(reps, pq), "no copy of the candidates");
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
