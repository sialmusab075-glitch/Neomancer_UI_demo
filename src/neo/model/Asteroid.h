#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace neo {

// One small body as SBDB describes it. The types follow the project rule that
// "unknown" is never 0: every value that JPL may report as null is an
// std::optional, and queries state explicitly what they do with an empty one.

enum class ObjectKind : std::uint8_t {
    Asteroid,
    Comet,
    Unknown,
};

// SBDB "kind" is two letters: a/c for asteroid/comet, n/u for numbered/unnumbered.
ObjectKind objectKindFromSbdb(const std::string& kind);
const char* toString(ObjectKind kind);

// SBDB reports the NEO and PHA flags as "Y", "N" or null. Null is not "no": it
// means SBDB does not assert the flag for this object (comets carry no PHA
// flag, and interstellar objects carry no NEO flag), so it stays distinct.
using Flag = std::optional<bool>;

struct ObjectClassification {
    ObjectKind  kind = ObjectKind::Unknown;
    bool        numbered = false;  // from the second letter of SBDB "kind"
    Flag        isNEO;
    Flag        isPHA;
    std::string orbitClass;        // SBDB "class": APO, ATE, AMO, IEO, HYA, ETc, ...
};

// Default albedo for the H -> diameter estimate. 0.14 is the value commonly
// used for NEO population estimates when no albedo is measured; it sits between
// the dark (~0.05) and bright (~0.25) ends, so an estimate can be wrong by a
// factor of ~2 in either direction. Documented in docs/NEO_PLAN.md.
constexpr double kDefaultAlbedo = 0.14;

// D[km] = 1329 / sqrt(p) * 10^(-H/5)  (Harris & Lagerros; the 1329 constant is
// the standard value derived from the solar constant and the magnitude system).
double diameterFromMagnitude(double absoluteMagnitudeH, double albedo = kDefaultAlbedo);

struct PhysicalProperties {
    std::optional<double> diameterKm;          // SBDB "diameter": a measured value only
    std::optional<double> diameterSigmaKm;     // SBDB "diameter_sigma"
    std::optional<double> absoluteMagnitudeH;  // SBDB "H"
    std::optional<double> albedo;              // SBDB "albedo"
    std::optional<double> rotationPeriodHours; // SBDB "rot_per"

    // Measured and estimated diameters are deliberately never merged into one
    // field: a query must be able to say which of the two it accepts.
    // estimatedDiameterKm() is empty when H is unknown (comets usually have no H).
    std::optional<double> estimatedDiameterKm() const;

    // Measured value if there is one, otherwise the H estimate; empty when
    // neither exists. Only for queries that opted into estimates.
    std::optional<double> bestDiameterKm() const;
};

struct OrbitalProperties {
    std::string orbitId;             // SBDB "orbit_id" (solution id, e.g. "659" or "JPL 16")
    double      epochJdTdb = 0.0;    // "epoch": epoch of osculation, JD (TDB)
    double      eccentricity = 0.0;  // "e"
    double      semiMajorAxisAU = 0.0; // "a" (negative for hyperbolic orbits)
    double      perihelionAU = 0.0;  // "q"
    double      inclinationDeg = 0.0;// "i", to the ecliptic
    double      ascendingNodeDeg = 0.0;     // "om" (Omega)
    double      argPerihelionDeg = 0.0;     // "w" (omega)
    double      meanAnomalyDeg = 0.0;       // "ma", at the epoch above
    double      meanMotionDegPerDay = 0.0;  // "n", as published (not derived from a)

    std::optional<double> periodDays;    // "per": absent for hyperbolic orbits
    std::optional<double> moidAU;        // "moid": Earth MOID
    std::optional<int>    conditionCode; // "condition_code": 0 (best) .. 9 (worst)

    // The two-body propagator in src/sim solves E - e sin E = M by Newton
    // iteration, which assumes a closed ellipse. Hyperbolic and parabolic
    // objects are stored and searchable, but never propagated or drawn.
    bool propagationSupported() const { return eccentricity < 1.0; }
};

struct Asteroid {
    std::string pdes;      // primary designation: the canonical key ("433", "2020 AN3", "2P")
    std::string spkid;     // SPK-ID, secondary index (arrives as a JSON number)
    std::string name;      // "Eros"; empty when unnamed
    std::string fullName;  // "   433 Eros (A898 PA)", as published (leading spaces kept)

    PhysicalProperties   physical;
    OrbitalProperties    orbital;
    ObjectClassification classification;

    // Display label: the name when there is one, otherwise the designation.
    const std::string& label() const { return name.empty() ? pdes : name; }
};

} // namespace neo
