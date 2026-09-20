#pragma once

#include "neo/ingest/ParseReport.h"
#include "neo/model/CloseApproach.h"

#include <string>
#include <vector>

namespace neo {

// CAD answers carry their own version, independent of the SBDB one.
constexpr const char* kCadApiVersion = "1.5";
constexpr const char* kCadDocUrl = "https://ssd-api.jpl.nasa.gov/doc/cad.html";

struct CadParseOptions {
    std::string expectedVersion = kCadApiVersion;
    // Rows must carry a designation, a date and a distance to be usable.
    // dist_min / dist_max / v_inf are filled from dist / v_rel when absent.
    bool requireDistance = true;
};

// Appends accepted rows to `out`; designations are resolved to objects later by
// Dataset::joinApproaches. Row defects are counted in `report`.
ParseStatus parseCadApproaches(const std::string& json, std::vector<ParsedApproach>& out, ValidationReport& report,
                               const CadParseOptions& options = CadParseOptions());

} // namespace neo
