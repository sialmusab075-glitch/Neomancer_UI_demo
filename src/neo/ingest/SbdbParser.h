#pragma once

#include "neo/ingest/ParseReport.h"
#include "neo/model/Asteroid.h"

#include <string>
#include <vector>

namespace neo {

// API signature version this build was written against. JPL documents that a
// different version means the format is not guaranteed, so the parser refuses
// the payload rather than guessing. Bumping it is a deliberate code change.
constexpr const char* kSbdbApiVersion = "1.0";
constexpr const char* kSbdbDocUrl = "https://ssd-api.jpl.nasa.gov/doc/sbdb_query.html";

// Fields neo_ingest asks SBDB for, in one place so the request and the parser
// cannot drift apart. Requested with full-prec=1.
extern const char* const kSbdbFields;

struct SbdbParseOptions {
    // Overridable so a test can prove the version check both accepts and rejects.
    std::string expectedVersion = kSbdbApiVersion;
    // A row missing any of pdes / epoch / e / a / q / i / om / w / ma / n cannot
    // be propagated or keyed, so it is rejected. Set false to keep such rows
    // out of the dataset silently (never used by the ingest tool).
    bool requireOrbit = true;
};

// Appends every accepted object to `out` (existing contents are kept, so pages
// accumulate). Row-level defects are counted in `report`, not fatal; a bad
// payload or an unexpected signature version returns a failed status.
ParseStatus parseSbdbObjects(const std::string& json, std::vector<Asteroid>& out, ValidationReport& report,
                             const SbdbParseOptions& options = SbdbParseOptions());

} // namespace neo
