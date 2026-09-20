#pragma once

#include "neo/model/Dataset.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {
namespace dsa {

// Bucket indexes: objects grouped by size class, approaches grouped by year.
//
// WHAT THEY ARE FOR
//   The two filters a user reaches for first ("show me the 140 m+ objects",
//   "show me the 2030s") are categorical, not numeric ranges. Answering those
//   by scanning 42,666 records, or even by two binary searches, is more work
//   than the question deserves: the answer is a precomputed, contiguous list.
//
// COMPLEXITY (n entries, b buckets)
//   build            O(n + b) time: one counting pass, a prefix sum, one
//                    placement pass. No comparisons and no sorting.
//   bucket(x)        O(1) to reach a contiguous slice of indices
//   range of years   O(1) per year, O(y) for y years
//   space            n * 4 bytes for the index array plus b offsets
//
// LAYOUT
//   Compressed sparse row: one flat std::uint32_t array holding every index,
//   grouped by bucket, plus an offsets array where bucket i occupies
//   [offset[i], offset[i + 1]). No vector-of-vectors, so the whole index is two
//   allocations and iterating a bucket is a straight walk through memory.
//
// SIZE CLASSES
//   These are APPLICATION-DEFINED categories, not NASA classifications. The
//   boundaries are chosen for the questions the UI asks:
//     < 10 m      the objects that burn up
//     10-50 m     Chelyabinsk (~20 m) to Tunguska (~50 m)
//     50-140 m    city-scale damage
//     140 m-1 km  the "potentially hazardous" size threshold used for survey
//                 completeness goals
//     > 1 km      global-effect objects
//     unknown     no diameter under the current policy: kept as its own bucket
//                 rather than dropped, so the UI can say how much it cannot
//                 classify instead of quietly under-reporting
//
// DIAMETER POLICY
//   A bucket index is built for ONE policy, because a measured-only index and a
//   measured-plus-estimated index put different objects in different buckets.
//   Mixing them would make "objects in the 140 m+ bucket" mean two things at
//   once, so the policy is stored in the index and reported with it.

enum class SizeBucket : std::uint8_t {
    Under10m = 0,
    From10To50m,
    From50To140m,
    From140mTo1km,
    Over1km,
    Unknown,
    Count,
};

constexpr std::size_t kSizeBucketCount = static_cast<std::size_t>(SizeBucket::Count);

// Upper bounds in km; the last class is everything above 1 km.
constexpr double kSizeBucketEdgesKm[4] = {0.01, 0.05, 0.14, 1.0};

const char* toString(SizeBucket bucket);

// Which diameters an index (or a query) is willing to use.
enum class DiameterPolicy : std::uint8_t {
    MeasuredOnly,      // only SBDB's measured diameter
    MeasuredOrEstimate // fall back to the H-based estimate
};

const char* toString(DiameterPolicy policy);

// The size class of one diameter in km.
SizeBucket sizeBucketOf(double diameterKm);

class SizeBucketIndex {
public:
    void build(const Dataset& dataset, DiameterPolicy policy);
    void clear();

    DiameterPolicy policy() const { return policy_; }
    std::size_t    size() const { return indices_.size(); }

    // The record indices in one bucket: a contiguous slice of the flat array.
    const std::uint32_t* begin(SizeBucket bucket) const;
    const std::uint32_t* end(SizeBucket bucket) const;
    std::size_t          count(SizeBucket bucket) const;

    // The bucket a record falls into under this index's policy.
    SizeBucket bucketOf(const Dataset& dataset, std::uint32_t recordIndex) const;

    bool checkInvariants(const Dataset& dataset, std::string& error) const;

private:
    std::vector<std::uint32_t> indices_;
    std::size_t                offsets_[kSizeBucketCount + 1] = {};
    DiameterPolicy             policy_ = DiameterPolicy::MeasuredOnly;
};

// Approaches grouped by calendar year (TDB). Same CSR layout; years are dense
// between the first and last year present, so a year maps to a slot by
// subtraction rather than a hash lookup.
class YearBucketIndex {
public:
    void build(const Dataset& dataset);
    void clear();

    int         firstYear() const { return firstYear_; }
    int         lastYear() const { return lastYear_; }
    std::size_t size() const { return indices_.size(); }
    std::size_t yearCount() const { return offsets_.empty() ? 0 : offsets_.size() - 1; }

    // Approach indices in one year.
    const std::uint32_t* begin(int year) const;
    const std::uint32_t* end(int year) const;
    std::size_t          count(int year) const;

    // fn(std::uint32_t approachIndex) for every approach in [firstYear, lastYear].
    template <class Fn>
    std::size_t forEachInYears(int from, int to, Fn&& fn) const {
        std::size_t visited = 0;
        if (offsets_.empty()) {
            return 0;
        }
        const int lo = from < firstYear_ ? firstYear_ : from;
        const int hi = to > lastYear_ ? lastYear_ : to;
        for (int year = lo; year <= hi; ++year) {
            for (const std::uint32_t* it = begin(year); it != end(year); ++it) {
                fn(*it);
                ++visited;
            }
        }
        return visited;
    }

    bool checkInvariants(const Dataset& dataset, std::string& error) const;

private:
    std::vector<std::uint32_t> indices_;
    std::vector<std::size_t>   offsets_; // yearCount() + 1 entries
    int                        firstYear_ = 0;
    int                        lastYear_ = -1;
};

// The calendar year (TDB) of a Julian Date, used by the year index.
int yearOfJulianDate(double jdTdb);

} // namespace dsa
} // namespace neo
