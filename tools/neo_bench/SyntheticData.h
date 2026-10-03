#pragma once

#include "neo/model/Dataset.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace neo {
namespace bench {

// Benchmark datasets bigger than the real catalogue (100k and 500k objects), and
// stand-ins for it when data/neo.db is absent. NEVER presented as real data: every
// dataset carries a kind and every CSV row repeats it.
//
//   synthetic-resampled   `resampleFrom` was given: each object is a copy of a randomly
//                         chosen REAL object (so every field distribution and every
//                         correlation, such as H with diameter or q with e, is the real
//                         one), with a fresh designation, and its approaches copied with
//                         the date moved by up to +-3 years.
//   synthetic-parametric  no real data: fixed, documented distributions chosen to look
//                         like the SBDB NEO population (see makeParametricObject). They
//                         are approximations; they exist so the harness can run anywhere.
//
// Deterministic: the same options give the same dataset on every compiler.
struct SyntheticOptions {
    std::size_t         objects = 1000;
    std::uint64_t       seed = 1;
    const Dataset*      resampleFrom = nullptr;
};

struct SyntheticInfo {
    std::string kind;          // synthetic-resampled | synthetic-parametric
    std::size_t objects = 0;
    std::size_t approaches = 0;
};

// Unique, designation-shaped, short enough for the small-string optimisation
// ("1950 AA", "2024 BC1234"). The mapping i -> string is injective.
std::string syntheticDesignation(std::size_t index);

Dataset makeSyntheticDataset(const SyntheticOptions& options, SyntheticInfo* info = nullptr);

// The first `objects` records of a dataset with their approaches: how the 1k and 10k sizes
// are cut from the real catalogue. Not synthetic: every value is the real one.
Dataset prefixOfDataset(const Dataset& source, std::size_t objects);

// The JSON text of one SBDB query response page holding the given objects, in the
// shape parseSbdbObjects reads. Used to time ingestion by page size without a network.
std::string makeSbdbPageJson(const Dataset& dataset, std::size_t firstRecord, std::size_t count);

} // namespace bench
} // namespace neo
