#pragma once

#include "neo/dsa/AvlTree.h"
#include "neo/dsa/BucketIndex.h"
#include "neo/dsa/HashMap.h"
#include "neo/dsa/Sort.h"
#include "neo/model/Dataset.h"
#include "neo/query/Histogram.h"
#include "neo/query/NameIndex.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace neo {

// Every index the query engine reads, built once after the dataset is loaded.
// After build() an IndexSet is immutable, which is what makes concurrent queries
// from several threads safe: nothing here is written by a query.

// NaN marks "unknown" in the columns below. Every range comparison against NaN
// is false, so an unknown value can never satisfy a range by accident; the one
// query that wants unknowns (DiameterMode::IncludeUnknown) asks for them by name.
constexpr double kUnknownValue = std::numeric_limits<double>::quiet_NaN();

constexpr std::uint16_t kNoClass = 0xFFFF;

struct IndexBuildInfo {
    std::string name;
    double      buildMs = 0.0;
    std::size_t bytes = 0;
    std::size_t entries = 0;
};

// The object fields a predicate reads, as dense columns. A residual check then
// touches a contiguous double array instead of a 400-byte record, and the
// numeric fields are already in the form the sorted views and histograms want.
struct ObjectColumns {
    std::vector<double> diameterMeasured; // SBDB's measured diameter, km
    std::vector<double> diameterBest;     // measured, else the H-based estimate
    std::vector<double> h;
    std::vector<double> moid;
    std::vector<double> a;
    std::vector<double> e;
    std::vector<double> i;
    std::vector<std::uint8_t>  kind;      // ObjectKind
    std::vector<std::uint8_t>  neo;       // 0 unknown, 1 yes, 2 no
    std::vector<std::uint8_t>  pha;
    std::vector<std::uint16_t> classId;   // index into IndexSet::classNames, or kNoClass
};

struct ApproachColumns {
    std::vector<double> jd;
    std::vector<double> dist;
    std::vector<double> vrel;
};

// Row counts for the categorical predicates, so their selectivity is exact.
struct CategoryStats {
    std::size_t kind[3] = {0, 0, 0};
    std::size_t neo[3] = {0, 0, 0};
    std::size_t pha[3] = {0, 0, 0};
    std::vector<std::size_t> classCounts;
    std::size_t grazing = 0;
};

struct IndexSet {
    const Dataset* dataset = nullptr;

    ObjectColumns   obj;
    ApproachColumns app;
    std::vector<std::string> classNames;
    CategoryStats   stats;

    // exact lookup
    dsa::HashMap<std::string, std::uint32_t> byDesignation;
    dsa::HashMap<std::string, std::uint32_t> bySpkId;
    NameIndex names;

    // ordered views (object universe)
    dsa::SortedView diameterMeasured, diameterBest, h, moid, a, e, i;
    // ordered views (approach universe)
    dsa::SortedView dist, vrel;
    dsa::AvlTree<double> dateTree;

    // categorical
    dsa::SizeBucketIndex sizeMeasured;
    dsa::SizeBucketIndex sizeBest;
    dsa::YearBucketIndex years;

    // estimation
    EquiDepthHistogram histDiameterMeasured, histDiameterBest, histH, histMoid, histA, histE, histI;
    EquiDepthHistogram histDate, histDist, histVrel;

    std::vector<IndexBuildInfo> report;
    double totalBuildMs = 0.0;
    std::size_t totalBytes = 0;

    void build(const Dataset& dataset);

    std::uint16_t classIdOf(const std::string& code) const; // kNoClass when the code never occurs
};

} // namespace neo
