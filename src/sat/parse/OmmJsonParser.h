#pragma once

#include "sat/model/ElementSet.h"
#include "sat/parse/ParseReport.h"

#include <string>
#include <vector>

namespace sat {

struct OmmParseResult {
    bool           ok = false;   // false: the payload is not an OMM JSON array at all
    std::string    error;
    SetParseReport report;
};

// CelesTrak's GP JSON (FORMAT=json): an array of OMM records, each with OBJECT_NAME, OBJECT_ID, EPOCH,
// MEAN_MOTION, ECCENTRICITY, INCLINATION, RA_OF_ASC_NODE, ARG_OF_PERICENTER, MEAN_ANOMALY, NORAD_CAT_ID,
// BSTAR, MEAN_MOTION_DOT, MEAN_MOTION_DDOT (required) and EPHEMERIS_TYPE, CLASSIFICATION_TYPE,
// ELEMENT_SET_NO, REV_AT_EPOCH (optional). A numeric field may be a JSON number or a numeric string.
// A single object is accepted as an array of one. Rows that are not usable are reported and skipped;
// accepted ones are appended to `out`. A body that is not JSON (CelesTrak answers "No GP data found"
// as plain text) is a failed result, not an empty one.
OmmParseResult parseOmmJson(const std::string& json, std::vector<ElementSet>& out);

} // namespace sat
