#pragma once

#include "neo/model/Dataset.h"
#include "neo/query/IndexSet.h"
#include "neo/query/Query.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neo {

// The query engine: one Query in, matching objects (with their matching
// approaches) out, plus an EXPLAIN of how the answer was found.
//
// USAGE
//     neo::Dataset dataset;  neo::loadDatabase(path, dataset, meta);
//     neo::QueryEngine engine(dataset);
//     engine.build();                         // once; ~0.2 s for 42k objects
//     neo::Query q;  q.pha = neo::TriState::Yes;  q.topK = 10; ...
//     neo::QueryResult r = engine.run(q);     // any thread, any number of times
//
// THREADING
//   build() must finish before the first run(). After that every method is const
//   and touches only immutable indexes plus locals, so any number of threads may
//   call run() concurrently on one engine with no locking. (The UI builds on a
//   worker thread and then queries from wherever it likes.) The Dataset must
//   outlive the engine and must not change.
//
// THREE EXECUTION PATHS, one answer
//   Naive       a full linear scan reading the records directly. The oracle: it
//               shares no index and no code with the other two, so a bug in an
//               index cannot hide in it.
//   FixedOrder  uses the indexes, but always drives from the same one (the first
//               present predicate in a fixed priority order) and applies the
//               rest in declaration order. What an index-aware engine with no
//               statistics would do; the benchmark baseline for the planner.
//   Planned     estimates every predicate's selectivity from equi-depth
//               histograms, drives from the index with the lowest estimated
//               total work, and orders the remaining predicates by rejection per
//               unit cost. See docs/NEO_PLAN.md, section "Query planner".
//   All three return the identical result for every valid query; the oracle
//   tests check exactly that.

enum class ExecMode : std::uint8_t { Naive, FixedOrder, Planned };
const char* toString(ExecMode mode);

// How the candidate stream is produced. Public so tests and the benchmark can
// force a specific one (run's `force` argument) and prove every path correct.
enum class Access : std::uint8_t {
    ScanObjects,     // every object, in index order
    ScanApproaches,  // every approach, in index order
    HashLookup,      // exact designation via the hash map
    NamePrefix,      // name/designation prefix via the sorted name index
    ObjectView,      // range slice of a sorted view over an object field
    SizeBuckets,     // the size-class buckets overlapping a diameter range
    DateTree,        // range walk of the AVL tree over approach dates
    YearBuckets,     // the year buckets overlapping a date window
    ApproachView,    // range slice of a sorted view over distance or velocity
};
const char* toString(Access access);

struct ResultRow {
    std::uint32_t object = 0;        // index into Dataset::records()
    std::uint32_t approachBegin = 0; // this row's matching approaches:
    std::uint32_t approachCount = 0; //   QueryResult::approachPool[begin .. begin + count)
    bool          sortKeyKnown = false;
    double        sortKey = 0.0;     // the value sorted on, when known
};

// One line of the EXPLAIN: a predicate, what the planner expected of it, and what
// actually happened when it ran.
struct StepStat {
    std::string   text;
    bool          approachUniverse = false;
    double        estSelectivity = 1.0; // estimated fraction of its universe that passes
    double        unitCost = 1.0;
    std::uint64_t evaluated = 0;
    std::uint64_t passed = 0;
};

// An alternative the planner weighed.
struct ConsideredPlan {
    std::string text;
    double      estCandidates = 0.0;
    double      estWork = 0.0;
    bool        chosen = false;
};

struct QueryStats {
    ExecMode    mode = ExecMode::Planned;
    std::string queryText;

    Access      driverAccess = Access::ScanObjects;
    std::string driverText;
    bool        driverApproachUniverse = false;
    bool        driverExact = true;       // false: a superset that the predicate then re-checks
    double      estCandidates = 0.0;
    std::uint64_t actualCandidates = 0;

    std::vector<StepStat>       steps;      // in the order they were applied
    std::vector<ConsideredPlan> considered; // cheapest first (Planned only)
    double estWork = 0.0;

    std::uint64_t predicateEvaluations = 0;
    std::size_t   matchedObjects = 0;
    std::size_t   matchedApproaches = 0;
    std::size_t   returnedObjects = 0;

    double planMs = 0.0;
    double execMs = 0.0;
    double totalMs = 0.0;

    // SQL-EXPLAIN-style text, ready to print or paste into a report.
    std::string explain() const;
};

struct QueryResult {
    bool                     ok = true;
    std::vector<std::string> errors;      // set when ok is false (validation failures)

    std::vector<ResultRow>   rows;        // best first, after sorting and top-K
    std::vector<std::uint32_t> approachPool;

    std::size_t totalObjects = 0;         // matches BEFORE top-K ("showing 10 of 1,324")
    std::size_t totalApproaches = 0;

    QueryStats stats;

    const std::uint32_t* approachesBegin(const ResultRow& row) const { return approachPool.data() + row.approachBegin; }
    const std::uint32_t* approachesEnd(const ResultRow& row) const {
        return approachPool.data() + row.approachBegin + row.approachCount;
    }
};

class QueryEngine {
public:
    explicit QueryEngine(const Dataset& dataset) : dataset_(&dataset) {}

    // Builds every index (and the per-index timing and memory report).
    void build();
    bool built() const { return built_; }

    // `force`, when set, drives the query from that access path if some
    // predicate can use it (otherwise the planner's own choice stands). It exists
    // so every path can be checked against the naive scan, and so the benchmark
    // can pin the driver; normal callers leave it empty.
    QueryResult run(const Query& query, ExecMode mode = ExecMode::Planned,
                    std::optional<Access> force = std::nullopt) const;

    // Search-box helpers.
    std::uint32_t findDesignation(const std::string& pdes) const;        // kInvalidRecord when absent
    std::uint32_t findSpkId(const std::string& spkid) const;
    std::vector<std::uint32_t> searchNames(const std::string& prefix, std::size_t limit = 0) const;

    const Dataset&  dataset() const { return *dataset_; }
    const IndexSet& indexes() const { return ix_; }
    const std::vector<IndexBuildInfo>& buildReport() const { return ix_.report; }
    double totalBuildMs() const { return ix_.totalBuildMs; }
    std::size_t totalIndexBytes() const { return ix_.totalBytes; }

private:
    const Dataset* dataset_;
    IndexSet       ix_;
    bool           built_ = false;
};

} // namespace neo
