#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace neo {
namespace bench {

using Clock = std::chrono::steady_clock;

// Median of the samples; the mean of the two middle ones for an even count.
// An empty input gives 0.
inline double medianOf(std::vector<double> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t mid = samples.size() / 2;
    return samples.size() % 2 == 1 ? samples[mid] : 0.5 * (samples[mid - 1] + samples[mid]);
}

struct Timing {
    std::vector<double> samples; // one per repeat, in the unit the caller asked for
    std::uint64_t result = 0;    // what the workload returned (the same on every repeat when stable)
    bool stable = true;          // false when two repeats returned different results

    int    repeats() const { return static_cast<int>(samples.size()); }
    double median() const { return medianOf(samples); }
    double min() const { return samples.empty() ? 0.0 : *std::min_element(samples.begin(), samples.end()); }
    double max() const { return samples.empty() ? 0.0 : *std::max_element(samples.begin(), samples.end()); }
};

// Runs `work` once untimed (warm caches, first-touch page faults), then `repeats`
// times timed with steady_clock. `work` returns a checksum of what it computed: it
// stops the compiler discarding the loop, and a checksum that differs between
// repeats flags a workload that is not deterministic. Each sample is nanoseconds
// per operation: the elapsed time of one repeat divided by `opsPerRepeat`, so a
// 50 ns lookup is measured over a batch, not by a 50 ns timer reading.
template <class Work>
Timing measureNsPerOp(int repeats, std::size_t opsPerRepeat, Work&& work) {
    Timing timing;
    const std::uint64_t expected = work();
    timing.result = expected;
    const double ops = opsPerRepeat == 0 ? 1.0 : static_cast<double>(opsPerRepeat);
    for (int r = 0; r < repeats; ++r) {
        const Clock::time_point start = Clock::now();
        const std::uint64_t got = work();
        const Clock::time_point stop = Clock::now();
        if (got != expected) {
            timing.stable = false;
        }
        timing.samples.push_back(std::chrono::duration<double, std::nano>(stop - start).count() / ops);
    }
    return timing;
}

// As above but each sample is milliseconds for the whole repeat.
template <class Work>
Timing measureMs(int repeats, Work&& work) {
    Timing timing = measureNsPerOp(repeats, 1, std::forward<Work>(work));
    for (double& sample : timing.samples) {
        sample /= 1.0e6;
    }
    return timing;
}

// FNV-1a over 64-bit values, for result checksums that depend on order.
inline std::uint64_t mixChecksum(std::uint64_t acc, std::uint64_t value) {
    acc ^= value;
    acc *= 1099511628211ull;
    return acc;
}
constexpr std::uint64_t kChecksumSeed = 14695981039346656037ull;

} // namespace bench
} // namespace neo
