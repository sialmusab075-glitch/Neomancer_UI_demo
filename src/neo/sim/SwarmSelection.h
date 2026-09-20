#pragma once

#include "neo/model/Dataset.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neo {

// Which objects the solar view's NEOS layer draws. All-or-nothing was the wrong
// control: 42,666 points is a very different frame budget from 1,000, and a small
// preset should still look like the population, not like the first rows of a table.
enum class SwarmPreset : std::uint8_t {
    PhaOnly,       // every potentially hazardous asteroid (2,549 in the current dataset)
    N1000,
    N5000,
    N20000,
    All,           // every object that can be propagated
    CurrentResult, // whatever the NEO FILTER's last query returned
};

constexpr int kSwarmPresetCount = 6;

// "PHAS ONLY", "1,000", "5,000", "20,000", "ALL", "FILTER RESULT".
const char* toString(SwarmPreset preset);
// The N of the numbered presets; 0 for the others.
std::size_t presetCount(SwarmPreset preset);

// The order the numbered presets take objects in, and why it is not table order:
//
//   RANK = diameter, largest first  (the measured diameter, else the H estimate;
//          objects with neither come last; ties broken by record index).
//
// It is deterministic and it makes the presets NESTED: 1,000 is inside 5,000 is
// inside 20,000 is inside All, so raising the count only ever adds points and
// nothing already on screen moves or vanishes. It is also the order that matters
// to a viewer: the biggest bodies are the ones worth seeing first. The price is
// that a small preset over-represents large objects, which the panel says
// ("LARGEST FIRST"); a uniform sample would look more like the population but
// would show a random 1,000 of mostly ~100 m rocks, each equally forgettable.
//
// Objects with e >= 1 (no closed orbit; none in the current dataset) are skipped
// everywhere, since the Kepler solver assumes an ellipse.
class SwarmCatalog {
public:
    explicit SwarmCatalog(const Dataset& dataset);

    std::size_t objectCount() const { return objects_; }      // every object in the dataset
    std::size_t propagatable() const { return rank_.size(); } // those with e < 1
    std::size_t phaCount() const { return pha_.size(); }

    // Record indices, in draw order. `current` (record indices in query-result
    // order) is only read for CurrentResult; objects that cannot be propagated are dropped.
    std::vector<std::uint32_t> select(SwarmPreset preset, const std::vector<std::uint32_t>* current = nullptr) const;

private:
    const Dataset* dataset_ = nullptr;
    std::size_t objects_ = 0;
    std::vector<std::uint32_t> rank_; // every propagatable record, by RANK
    std::vector<std::uint32_t> pha_;  // the PHAs among them, by RANK
};

} // namespace neo
