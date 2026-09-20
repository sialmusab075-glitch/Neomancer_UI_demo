#pragma once

#include <cstdint>

namespace neo {
namespace dsa {

// Operation counters for the stage 7 report: how many comparisons a sort really
// made, how many probes a lookup cost, how many rotations a tree needed.
//
// Every structure takes a Counters policy as its last template parameter and
// inherits from it privately. With NullCounters (the default) each call is an
// inline empty function and the empty base class takes no space, so an
// instrumented and an uninstrumented build have identical layout and generated
// code. Tests instantiate the same structures with LiveCounters, so the
// counting path is exercised without a second build.

// The methods are const and the fields mutable, so counting also works from a
// const lookup (a find() is logically const but still costs probes).
struct NullCounters {
    static constexpr bool enabled = false;

    void comparison(std::uint64_t = 1) const noexcept {}
    void probe(std::uint64_t = 1) const noexcept {}
    void swapOp(std::uint64_t = 1) const noexcept {}
    void rotation(std::uint64_t = 1) const noexcept {}
    void moveOp(std::uint64_t = 1) const noexcept {}
    void resetCounters() noexcept {}
};

struct LiveCounters {
    static constexpr bool enabled = true;

    mutable std::uint64_t comparisons = 0;
    mutable std::uint64_t probes = 0;
    mutable std::uint64_t swaps = 0;
    mutable std::uint64_t rotations = 0;
    mutable std::uint64_t moves = 0;

    void comparison(std::uint64_t n = 1) const noexcept { comparisons += n; }
    void probe(std::uint64_t n = 1) const noexcept { probes += n; }
    void swapOp(std::uint64_t n = 1) const noexcept { swaps += n; }
    void rotation(std::uint64_t n = 1) const noexcept { rotations += n; }
    void moveOp(std::uint64_t n = 1) const noexcept { moves += n; }
    void resetCounters() noexcept { *this = LiveCounters(); }
};

} // namespace dsa
} // namespace neo
