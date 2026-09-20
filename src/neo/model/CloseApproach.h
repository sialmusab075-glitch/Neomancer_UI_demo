#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace neo {

// Sentinel for "this approach has no parent object in the dataset". Rows that
// keep it are counted in the join report and dropped, never silently ignored.
constexpr std::uint32_t kNoObject = 0xFFFFFFFFu;

// One encounter from the CAD API. An object has many of these, so the record
// holds a range into one flat vector rather than its own container: see
// Dataset. The struct is kept small and trivially copyable because there can be
// millions of these, and the sorted/bucketed approach views index into them.
struct CloseApproach {
    std::uint32_t objectIndex = kNoObject; // index into Dataset::records()
    double jdTdb = 0.0;          // "jd": time of close approach, JD (TDB)
    double distanceAU = 0.0;     // "dist": nominal approach distance
    double distanceMinAU = 0.0;  // "dist_min": minimum (3-sigma)
    double distanceMaxAU = 0.0;  // "dist_max": maximum (3-sigma)
    double relVelocityKms = 0.0; // "v_rel": relative to the approach body

    // Optional columns: "h" is absent for some comets; "diameter" only exists
    // when the request asked for it, and is null when no diameter is known.
    // v_inf is NOT filled in from v_rel: the two are different quantities
    // (v_inf excludes the body's gravitational focusing, so v_inf < v_rel), and
    // substituting one for the other would silently corrupt a velocity query.
    std::optional<double> vInfinityKms;      // "v_inf": relative to a massless body
    std::optional<double> absoluteMagnitudeH;
    std::optional<double> diameterKm;
    std::optional<double> diameterSigmaKm;

    // True when dist_min / dist_max were absent and the nominal distance stands
    // in for them. Unlike v_inf this fallback is kept, because a range query on
    // distance must not see a spurious 0 AU, but it is counted in the
    // ValidationReport so the substitution is never invisible.
    bool distRangeDerived = false;

    bool matched() const { return objectIndex != kNoObject; }
};

// What a CAD row carried before the join: the designation is resolved to an
// object index by Dataset, then dropped. Kept separate so CloseApproach itself
// never stores a string.
struct ParsedApproach {
    std::string   designation; // CAD "des", joins to SBDB "pdes"
    std::string   orbitId;     // CAD "orbit_id" of the solution used
    CloseApproach approach;
};

} // namespace neo
