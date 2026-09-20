#pragma once

#include <cstddef>
#include <vector>

namespace neo {

// Equi-depth (equal-frequency) histogram, used by the planner to estimate how
// selective a range predicate is WITHOUT running it.
//
// WHY EQUI-DEPTH AND NOT EQUI-WIDTH
//   The data is heavily skewed: most objects are small, a few are huge; most
//   approaches sit near the 0.05 au cut-off and few are extremely close. Equal-
//   width bins would put nearly everything in one bin and give a uselessly
//   coarse estimate exactly where the data is dense. Equal-frequency bins put
//   the boundaries where the data is, so every bin holds the same share of the
//   rows and the estimate's worst-case error is one bin (1/64 of the rows, ~1.6%)
//   wherever the range falls.
//
// SHAPE
//   bins + 1 edges taken from a sorted key array: edge k is the key at rank
//   k * (n - 1) / bins. Bin b covers [edge b, edge b + 1] and holds ~n / bins
//   values. Repeated edges are expected and meaningful: a run of identical edges
//   is a value so common (whole-degree inclinations, a shared diameter) that it
//   fills whole bins, and the estimate for that exact value reflects it.
//
// ESTIMATE
//   fraction(lo <= v <= hi) = cdf(<= hi) - cdf(< lo), each read off the edges
//   with linear interpolation inside the bin that contains the point. Multiplied
//   by the known fraction, so unknown values (which no range matches) never
//   inflate an estimate.
//
// COST
//   build: O(bins) from an already-sorted array, which the sorted views provide
//   for free. estimate: two O(log bins) binary searches. space: (bins + 1) doubles.

class EquiDepthHistogram {
public:
    static constexpr std::size_t kDefaultBins = 64;

    // sortedKeys: the known values in ascending order (unknowns excluded).
    // totalCount: every row in the universe, known or not.
    void build(const double* sortedKeys, std::size_t knownCount, std::size_t totalCount,
               std::size_t bins = kDefaultBins);

    // Estimated fraction of ALL rows (not just known ones) with lo <= value <= hi.
    double selectivity(double lo, double hi) const;

    double      knownFraction() const {
        return totalCount_ == 0 ? 0.0 : static_cast<double>(knownCount_) / static_cast<double>(totalCount_);
    }
    std::size_t knownCount() const { return knownCount_; }
    std::size_t totalCount() const { return totalCount_; }
    std::size_t bins() const { return edges_.empty() ? 0 : edges_.size() - 1; }
    const std::vector<double>& edges() const { return edges_; }
    std::size_t memoryBytes() const { return edges_.capacity() * sizeof(double); }

private:
    double cdfLess(double x) const;      // fraction of known values strictly below x
    double cdfLessEqual(double x) const; // fraction of known values at or below x

    std::vector<double> edges_;
    std::size_t knownCount_ = 0;
    std::size_t totalCount_ = 0;
};

} // namespace neo
