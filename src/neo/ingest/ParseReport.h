#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neo {

// Result of a parse call. Parsing never throws across this boundary: a bad
// payload is an expected outcome (truncated download, changed API version,
// error document served instead of data), so it is reported, not signalled.
struct ParseStatus {
    bool        ok = false;
    std::string error; // empty when ok

    explicit operator bool() const { return ok; }

    static ParseStatus success() { return ParseStatus{true, std::string()}; }
    static ParseStatus failure(std::string message) { return ParseStatus{false, std::move(message)}; }
};

// Why one row was dropped. Kept as a sample, not one entry per row, so a file
// full of the same defect cannot blow up memory.
struct RejectedRow {
    std::size_t index = 0; // row number in the response, 0-based
    std::string reason;
};

// Per-response accounting, printed by neo_ingest and asserted in tests.
struct ValidationReport {
    std::string signatureSource;
    std::string signatureVersion;

    // What the response said about itself: "count" is the rows it carries,
    // "total" (CAD, only when a limit was given) is how many exist in total.
    // Paging compares the two instead of re-parsing the payload.
    std::size_t declaredCount = 0;
    std::optional<std::size_t> declaredTotal;

    std::size_t rowsSeen = 0;
    std::size_t rowsAccepted = 0;
    std::size_t rowsRejected = 0;
    // CAD rows whose dist_min / dist_max were missing and were filled in from
    // the nominal distance (see CloseApproach::distRangeDerived).
    std::size_t derivedDistanceRanges = 0;

    // Null count per response column, in the order the response declared them.
    std::vector<std::pair<std::string, std::size_t>> nullCounts;
    std::vector<RejectedRow> rejectedSamples;

    static constexpr std::size_t kMaxRejectedSamples = 20;

    void noteRejected(std::size_t index, std::string reason);
    std::size_t nullCount(const std::string& field) const;
    std::string toString() const;
    void merge(const ValidationReport& other); // accumulate across pages
};

} // namespace neo
