#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sat {

// What a parser did with its input: how many element sets it kept, and why it dropped the others.
// Kept as samples, not one entry per row, so a file full of the same defect cannot blow up memory.
struct SetParseReport {
    std::size_t seen = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    // (position in the input: 0-based set index, reason)
    std::vector<std::pair<std::size_t, std::string>> rejectedSamples;

    static constexpr std::size_t kMaxSamples = 20;

    void reject(std::size_t index, std::string reason) {
        ++rejected;
        if (rejectedSamples.size() < kMaxSamples) {
            rejectedSamples.emplace_back(index, std::move(reason));
        }
    }
};

} // namespace sat
