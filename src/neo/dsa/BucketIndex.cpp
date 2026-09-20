#include "neo/dsa/BucketIndex.h"

#include "sim/SimClock.h"

#include <optional>

namespace neo {
namespace dsa {

namespace {

std::optional<double> diameterUnder(const Asteroid& object, DiameterPolicy policy) {
    if (policy == DiameterPolicy::MeasuredOnly) {
        return object.physical.diameterKm;
    }
    return object.physical.bestDiameterKm();
}

} // namespace

const char* toString(SizeBucket bucket) {
    switch (bucket) {
    case SizeBucket::Under10m:      return "< 10 m";
    case SizeBucket::From10To50m:   return "10-50 m";
    case SizeBucket::From50To140m:  return "50-140 m";
    case SizeBucket::From140mTo1km: return "140 m - 1 km";
    case SizeBucket::Over1km:       return "> 1 km";
    case SizeBucket::Unknown:       return "unknown";
    case SizeBucket::Count:         break;
    }
    return "?";
}

const char* toString(DiameterPolicy policy) {
    return policy == DiameterPolicy::MeasuredOnly ? "measured only" : "measured or H-estimate";
}

SizeBucket sizeBucketOf(double diameterKm) {
    if (diameterKm < kSizeBucketEdgesKm[0]) {
        return SizeBucket::Under10m;
    }
    if (diameterKm < kSizeBucketEdgesKm[1]) {
        return SizeBucket::From10To50m;
    }
    if (diameterKm < kSizeBucketEdgesKm[2]) {
        return SizeBucket::From50To140m;
    }
    if (diameterKm < kSizeBucketEdgesKm[3]) {
        return SizeBucket::From140mTo1km;
    }
    return SizeBucket::Over1km;
}

int yearOfJulianDate(double jdTdb) { return sim::calendarFromJulianDate(jdTdb).year; }

// --- size buckets -----------------------------------------------------------

void SizeBucketIndex::build(const Dataset& dataset, DiameterPolicy policy) {
    policy_ = policy;
    const std::size_t n = dataset.records().size();
    indices_.assign(n, 0);
    for (std::size_t i = 0; i <= kSizeBucketCount; ++i) {
        offsets_[i] = 0;
    }
    if (n == 0) {
        return;
    }

    // Counting sort: count, prefix-sum into offsets, then place. O(n + b).
    std::size_t counts[kSizeBucketCount] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const std::optional<double> diameter = diameterUnder(dataset.records()[i].object, policy);
        const SizeBucket bucket = diameter ? sizeBucketOf(*diameter) : SizeBucket::Unknown;
        ++counts[static_cast<std::size_t>(bucket)];
    }
    std::size_t running = 0;
    for (std::size_t i = 0; i < kSizeBucketCount; ++i) {
        offsets_[i] = running;
        running += counts[i];
    }
    offsets_[kSizeBucketCount] = running;

    std::size_t cursor[kSizeBucketCount];
    for (std::size_t i = 0; i < kSizeBucketCount; ++i) {
        cursor[i] = offsets_[i];
    }
    // Records are placed in index order, so each bucket stays ascending: a
    // bucket's contents are directly comparable with a linear scan's output.
    for (std::size_t i = 0; i < n; ++i) {
        const std::optional<double> diameter = diameterUnder(dataset.records()[i].object, policy);
        const SizeBucket bucket = diameter ? sizeBucketOf(*diameter) : SizeBucket::Unknown;
        indices_[cursor[static_cast<std::size_t>(bucket)]++] = static_cast<std::uint32_t>(i);
    }
}

void SizeBucketIndex::clear() {
    indices_.clear();
    for (std::size_t i = 0; i <= kSizeBucketCount; ++i) {
        offsets_[i] = 0;
    }
}

const std::uint32_t* SizeBucketIndex::begin(SizeBucket bucket) const {
    return indices_.data() + offsets_[static_cast<std::size_t>(bucket)];
}

const std::uint32_t* SizeBucketIndex::end(SizeBucket bucket) const {
    return indices_.data() + offsets_[static_cast<std::size_t>(bucket) + 1];
}

std::size_t SizeBucketIndex::count(SizeBucket bucket) const {
    return offsets_[static_cast<std::size_t>(bucket) + 1] - offsets_[static_cast<std::size_t>(bucket)];
}

SizeBucket SizeBucketIndex::bucketOf(const Dataset& dataset, std::uint32_t recordIndex) const {
    if (recordIndex >= dataset.records().size()) {
        return SizeBucket::Unknown;
    }
    const std::optional<double> diameter = diameterUnder(dataset.records()[recordIndex].object, policy_);
    return diameter ? sizeBucketOf(*diameter) : SizeBucket::Unknown;
}

bool SizeBucketIndex::checkInvariants(const Dataset& dataset, std::string& error) const {
    if (indices_.size() != dataset.records().size()) {
        error = "the index holds " + std::to_string(indices_.size()) + " entries for " +
                std::to_string(dataset.records().size()) + " records";
        return false;
    }
    std::vector<bool> seen(dataset.records().size(), false);
    for (std::size_t b = 0; b < kSizeBucketCount; ++b) {
        const SizeBucket bucket = static_cast<SizeBucket>(b);
        const std::uint32_t* previous = nullptr;
        for (const std::uint32_t* it = begin(bucket); it != end(bucket); ++it) {
            if (*it >= seen.size() || seen[*it]) {
                error = "record " + std::to_string(*it) + " appears twice or is out of range";
                return false;
            }
            seen[*it] = true;
            if (previous != nullptr && *previous > *it) {
                error = "bucket contents are not in ascending record order";
                return false;
            }
            previous = it;
            if (bucketOf(dataset, *it) != bucket) {
                error = "record " + std::to_string(*it) + " is filed in the wrong bucket";
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            error = "record " + std::to_string(i) + " is in no bucket";
            return false;
        }
    }
    return true;
}

// --- year buckets -----------------------------------------------------------

void YearBucketIndex::build(const Dataset& dataset) {
    clear();
    const std::vector<CloseApproach>& approaches = dataset.approaches();
    if (approaches.empty()) {
        return;
    }

    std::vector<int> years(approaches.size());
    int lo = yearOfJulianDate(approaches[0].jdTdb);
    int hi = lo;
    for (std::size_t i = 0; i < approaches.size(); ++i) {
        years[i] = yearOfJulianDate(approaches[i].jdTdb);
        lo = years[i] < lo ? years[i] : lo;
        hi = years[i] > hi ? years[i] : hi;
    }
    firstYear_ = lo;
    lastYear_ = hi;

    const std::size_t slots = static_cast<std::size_t>(hi - lo) + 1;
    std::vector<std::size_t> counts(slots, 0);
    for (const int year : years) {
        ++counts[static_cast<std::size_t>(year - lo)];
    }
    offsets_.assign(slots + 1, 0);
    std::size_t running = 0;
    for (std::size_t i = 0; i < slots; ++i) {
        offsets_[i] = running;
        running += counts[i];
    }
    offsets_[slots] = running;

    std::vector<std::size_t> cursor(offsets_.begin(), offsets_.end() - 1);
    indices_.assign(approaches.size(), 0);
    for (std::size_t i = 0; i < approaches.size(); ++i) {
        const std::size_t slot = static_cast<std::size_t>(years[i] - lo);
        indices_[cursor[slot]++] = static_cast<std::uint32_t>(i);
    }
}

void YearBucketIndex::clear() {
    indices_.clear();
    offsets_.clear();
    firstYear_ = 0;
    lastYear_ = -1;
}

const std::uint32_t* YearBucketIndex::begin(int year) const {
    if (offsets_.empty() || year < firstYear_ || year > lastYear_) {
        return indices_.data();
    }
    return indices_.data() + offsets_[static_cast<std::size_t>(year - firstYear_)];
}

const std::uint32_t* YearBucketIndex::end(int year) const {
    if (offsets_.empty() || year < firstYear_ || year > lastYear_) {
        return indices_.data(); // begin() == end(): an empty range
    }
    return indices_.data() + offsets_[static_cast<std::size_t>(year - firstYear_) + 1];
}

std::size_t YearBucketIndex::count(int year) const {
    return static_cast<std::size_t>(end(year) - begin(year));
}

bool YearBucketIndex::checkInvariants(const Dataset& dataset, std::string& error) const {
    if (indices_.size() != dataset.approaches().size()) {
        error = "the year index holds " + std::to_string(indices_.size()) + " entries for " +
                std::to_string(dataset.approaches().size()) + " approaches";
        return false;
    }
    if (indices_.empty()) {
        return true;
    }
    std::vector<bool> seen(indices_.size(), false);
    for (int year = firstYear_; year <= lastYear_; ++year) {
        for (const std::uint32_t* it = begin(year); it != end(year); ++it) {
            if (*it >= seen.size() || seen[*it]) {
                error = "approach " + std::to_string(*it) + " appears twice or is out of range";
                return false;
            }
            seen[*it] = true;
            if (yearOfJulianDate(dataset.approaches()[*it].jdTdb) != year) {
                error = "approach " + std::to_string(*it) + " is filed under the wrong year";
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            error = "approach " + std::to_string(i) + " is in no year bucket";
            return false;
        }
    }
    return true;
}

} // namespace dsa
} // namespace neo
