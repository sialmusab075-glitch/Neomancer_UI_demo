#include "neo/query/Histogram.h"

#include "neo/dsa/Sort.h"

#include <algorithm>
#include <cmath>

namespace neo {

void EquiDepthHistogram::build(const double* sortedKeys, std::size_t knownCount, std::size_t totalCount,
                               std::size_t bins) {
    edges_.clear();
    knownCount_ = knownCount;
    totalCount_ = totalCount;
    if (knownCount == 0 || bins == 0) {
        return;
    }
    // Never more bins than values: a bin narrower than one row is meaningless.
    const std::size_t b = std::min(bins, knownCount);
    edges_.resize(b + 1);
    for (std::size_t k = 0; k <= b; ++k) {
        edges_[k] = sortedKeys[k * (knownCount - 1) / b];
    }
}

double EquiDepthHistogram::cdfLess(double x) const {
    const std::size_t b = bins();
    if (b == 0 || std::isnan(x) || !(x > edges_[0])) {
        return 0.0;
    }
    if (x > edges_[b]) {
        return 1.0;
    }
    // First edge >= x; x > edges_[0] and x <= edges_[b] keep j in [1, b].
    const std::size_t j = dsa::lowerBound(edges_.data(), edges_.size(), x);
    const double lo = edges_[j - 1];
    const double hi = edges_[j];
    const double frac = hi > lo ? (x - lo) / (hi - lo) : 1.0;
    return (static_cast<double>(j - 1) + frac) / static_cast<double>(b);
}

double EquiDepthHistogram::cdfLessEqual(double x) const {
    const std::size_t b = bins();
    if (b == 0 || std::isnan(x) || x < edges_[0]) {
        return 0.0;
    }
    if (x >= edges_[b]) {
        return 1.0;
    }
    // First edge > x; edges_[0] <= x < edges_[b] keeps j in [1, b].
    const std::size_t j = dsa::upperBound(edges_.data(), edges_.size(), x);
    const double lo = edges_[j - 1];
    const double hi = edges_[j];
    const double frac = hi > lo ? (x - lo) / (hi - lo) : 0.0;
    return (static_cast<double>(j - 1) + frac) / static_cast<double>(b);
}

double EquiDepthHistogram::selectivity(double lo, double hi) const {
    if (bins() == 0 || lo > hi) {
        return 0.0;
    }
    const double inRange = std::clamp(cdfLessEqual(hi) - cdfLess(lo), 0.0, 1.0);
    return inRange * knownFraction();
}

} // namespace neo
