#pragma once

#include "neo/model/Asteroid.h"
#include "neo/model/CloseApproach.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace neo {

// One object plus the range of encounters that belong to it. The approaches
// themselves live in one flat vector owned by Dataset:
//
//   records_[i].firstApproach .. +approachCount   ->  approaches_[...]
//
// so every index built later (date order, distance order, year buckets) stores
// plain uint32_t offsets into that vector instead of pointers, and the whole
// dataset stays two allocations rather than one per object.
struct AsteroidRecord {
    Asteroid      object;
    std::uint32_t firstApproach = 0;
    std::uint32_t approachCount = 0;
};

// A contiguous view of one object's approaches (empty when it has none).
struct ApproachSpan {
    const CloseApproach* data = nullptr;
    std::size_t          count = 0;

    const CloseApproach* begin() const { return data; }
    const CloseApproach* end() const { return data + count; }
    bool  empty() const { return count == 0; }
    const CloseApproach& operator[](std::size_t i) const { return data[i]; }
};

// Outcome of joining CAD rows onto SBDB objects by designation.
struct JoinReport {
    std::size_t rowsSeen = 0;
    std::size_t rowsMatched = 0;
    std::size_t rowsUnmatched = 0;
    std::size_t objectsWithApproaches = 0;
    // Distinct designations that matched no object, with how many rows each had.
    // Capped at kMaxUnmatchedSamples entries; distinctUnmatched is the true total.
    std::vector<std::pair<std::string, std::size_t>> unmatchedSamples;
    std::size_t distinctUnmatched = 0;

    static constexpr std::size_t kMaxUnmatchedSamples = 20;
    std::string toString() const;
};

constexpr std::uint32_t kInvalidRecord = 0xFFFFFFFFu;

// The master collection: one copy of each object, one flat vector of approaches.
// Nothing else in the system owns object data; indexes and query results carry
// uint32_t positions into records() and approaches().
class Dataset {
public:
    // Takes ownership of the parsed objects. Later objects with a designation
    // that is already present are skipped and counted in duplicatesDropped().
    void setObjects(std::vector<Asteroid>&& objects);

    // Resolves each row's designation to a record, sorts the rows by
    // (object, date) and fills in every record's approach range. Rows whose
    // designation is unknown are dropped and reported. Consumes `rows`.
    JoinReport joinApproaches(std::vector<ParsedApproach>&& rows);

    // Loading from storage: the approaches already carry a resolved
    // objectIndex, so no join is needed. Rows pointing outside records() are
    // dropped and the count returned. Consumes `approaches`.
    std::size_t setApproaches(std::vector<CloseApproach>&& approaches);

    const std::vector<AsteroidRecord>& records() const { return records_; }
    const std::vector<CloseApproach>&  approaches() const { return approaches_; }

    std::size_t objectCount() const { return records_.size(); }
    std::size_t approachCount() const { return approaches_.size(); }
    std::size_t duplicatesDropped() const { return duplicatesDropped_; }

    // Canonical lookup by primary designation; kInvalidRecord when absent.
    // Stage 5 replaces the map behind this call with neo::HashMap, which is why
    // callers only ever see the index, never the container.
    std::uint32_t find(const std::string& pdes) const;
    // Secondary lookup by SPK-ID.
    std::uint32_t findBySpkId(const std::string& spkid) const;

    ApproachSpan approachesOf(std::uint32_t recordIndex) const;

    void clear();

private:
    // Sorts approaches_ by (object, date) and refills every record's range.
    // One sort gives both the contiguous per-record block and chronological
    // order inside it; every caller that touches approaches_ goes through here,
    // so the range invariant lives in exactly one place.
    void rebuildRanges(std::size_t& objectsWithApproaches);

    std::vector<AsteroidRecord> records_;
    std::vector<CloseApproach>  approaches_;
    std::unordered_map<std::string, std::uint32_t> byDesignation_;
    std::unordered_map<std::string, std::uint32_t> bySpkId_;
    std::size_t duplicatesDropped_ = 0;
};

} // namespace neo
