#pragma once

#include "Recorder.h"
#include "neo/model/Dataset.h"
#include "neo/query/QueryEngine.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {
namespace bench {

// The experiments of docs/NEO_PLAN.md section 8. Each takes a built QueryEngine over a
// dataset, times every contender on the SAME work, checks that the contenders agree,
// and appends rows to the Recorder. A disagreement is recorded with Recorder::fail:
// timings of variants that compute different answers mean nothing.
//
// Timing rules (Timing.h): one untimed warm-up, then `repeats` timed repeats (never
// fewer than 5), steady_clock, the MEDIAN reported with min and max beside it. A
// figure that costs tens of nanoseconds is measured over a batch of operations and
// divided down, so the clock resolution does not decide the result.
struct ExperimentConfig {
    int           repeats = 7;
    std::uint64_t seed = 42;
    // A linear scan over 500k objects per lookup would take minutes at 2,000 lookups;
    // linear variants stop after this many element visits per repeat (so they run fewer
    // operations) and the row says how many. Figures are per operation either way.
    std::size_t   linearBudget = 20000000;
    std::size_t   lookupQueries = 2000;
    std::size_t   rangeWindows = 20;
};

struct DatasetUnderTest {
    std::string         name;  // "real", "synthetic-100000", "fixture"
    std::string         kind;  // real | synthetic-resampled | synthetic-parametric
    const Dataset*      dataset = nullptr;
    const QueryEngine*  engine = nullptr; // built
};

int effectiveRepeats(const ExperimentConfig& config);

// exact lookup by designation: linear scan, dsa::HashMap, std::unordered_map, dsa::IndexedHashMap
// (the record-index map of the memory experiment): build time, lookup time, table memory.
void runLookupExperiment(const DatasetUnderTest& data, const ExperimentConfig& config, Recorder& out);

// approach date range at 0.1 %, 1 % and 10 % selectivity: linear scan, sorted view + binary
// search, AVL tree; build time, query time, memory.
void runRangeExperiment(const DatasetUnderTest& data, const ExperimentConfig& config, Recorder& out);

// mutation: what the AVL tree is FOR. Insert and erase cost on the AVL tree against keeping a sorted array
// current (shifting on each insert/erase; merging a batch; rebuilding it all), then the number of range
// queries per mutation at which the two structures cost the same overall. The range experiment alone
// shows only the query half of the trade.
void runMutationExperiment(const DatasetUnderTest& data, const ExperimentConfig& config, Recorder& out);

// top-K by approach distance, K = 10, 100, 1000: std::sort, dsa::mergeSort, std::partial_sort,
// dsa::topK (heap), std::priority_queue.
void runTopKExperiment(const DatasetUnderTest& data, const ExperimentConfig& config, Recorder& out);

// eight representative queries through QueryEngine::run in each execution mode: naive scan,
// fixed driver order, statistics-driven planner.
void runQueryExperiment(const DatasetUnderTest& data, const ExperimentConfig& config, Recorder& out);

// Index build time (the engine is rebuilt `repeats` times), memory of every index, and the
// master vectors.
void runMemoryExperiments(const DatasetUnderTest& data, const ExperimentConfig& config, Recorder& out);

// Parsing time of SBDB response pages of different sizes over a fixed number of objects.
// Network time is not in it (there is none); see the note on every row.
void runIngestExperiment(const ExperimentConfig& config, std::size_t totalObjects,
                         const std::vector<std::size_t>& pageSizes, Recorder& out);

} // namespace bench
} // namespace neo
