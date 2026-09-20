#pragma once

#include "neo/model/Asteroid.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neo {

// The query API. A Query is plain data: the NEO FILTER panel, the neo_query CLI
// and the tests all fill in one of these and hand it to QueryEngine::run. No
// string parsing is involved, and nothing here knows about SQL.
//
// UNITS (fixed, so no field carries a unit suffix in a widget's head):
//   dates      Julian Date, TDB      (neo::julianDateFromIsoDate converts)
//   distances  AU                    (kLunarDistanceAU converts from LD)
//   velocities km/s
//   diameters  km
//   angles     degrees
//
// SEMANTICS
//   * Every bound is INCLUSIVE. A range with lo > hi is a validation error, not
//     an empty result: it is almost certainly a mistake in the caller.
//   * A missing bound is open on that side; a Range with neither bound is not a
//     filter at all.
//   * Unknown is not zero. A range on H, MOID or diameter never matches an object
//     whose value is unknown (except under DiameterMode::IncludeUnknown, which
//     asks for exactly that).
//   * All approach conditions apply to ONE approach row: an object matches only
//     if a single one of its approaches satisfies every approach condition. Two
//     different approaches, one meeting the distance limit and the other the
//     velocity limit, do not combine.
//   * A result is an OBJECT, together with the approaches that matched. With no
//     approach condition set, an object matches on its own properties and all of
//     its approaches are listed (possibly none).

constexpr double kLunarDistanceAU = 384400.0 / 149597870.7; // 1 LD, ~0.00257 AU

struct Range {
    std::optional<double> lo;
    std::optional<double> hi;

    bool active() const { return lo.has_value() || hi.has_value(); }

    static Range between(double low, double high) {
        Range r;
        r.lo = low;
        r.hi = high;
        return r;
    }
    static Range atLeast(double low) {
        Range r;
        r.lo = low;
        return r;
    }
    static Range atMost(double high) {
        Range r;
        r.hi = high;
        return r;
    }
};

// Any = no condition; Yes / No test a flag that SBDB reported. An object whose
// flag is not asserted (null) matches neither Yes nor No: unknown is not "no".
enum class TriState : std::uint8_t { Any, Yes, No };

// Which diameter a diameter condition (and a diameter sort) looks at.
enum class DiameterMode : std::uint8_t {
    MeasuredOrEstimated, // the measured value, else the H-based estimate; objects with neither are excluded
    MeasuredOnly,        // only SBDB's measured diameter; estimates are ignored
    IncludeUnknown,      // as MeasuredOrEstimated, but objects with no diameter at all also match
};

enum class SortField : std::uint8_t {
    None,              // object index order
    Designation,
    Diameter,          // per DiameterMode
    AbsoluteMagnitude,
    Moid,
    SemiMajorAxis,
    Eccentricity,
    Inclination,
    Date,              // approach fields: an object's key is its best matching approach
    Distance,
    Velocity,
};

enum class SortDirection : std::uint8_t { Ascending, Descending };

const char* toString(TriState value);
const char* toString(DiameterMode mode);
const char* toString(SortField field);

// True for the sort fields that are properties of an approach rather than of an
// object. For those an object's key is the minimum (ascending) or maximum
// (descending) over its MATCHING approaches.
bool isApproachSort(SortField field);

struct Query {
    // --- object filters -----------------------------------------------------
    std::string designation;                 // exact primary designation, case-sensitive
    std::string namePrefix;                  // case-insensitive prefix of the name OR the designation
    std::optional<ObjectKind> kind;
    TriState neo = TriState::Any;
    TriState pha = TriState::Any;
    std::vector<std::string> orbitClasses;   // any-of, exact codes ("APO", "ATE", ...)
    Range diameterKm;
    DiameterMode diameterMode = DiameterMode::MeasuredOrEstimated;
    Range absoluteMagnitude;                 // H
    Range moidAU;
    Range semiMajorAxisAU;
    Range eccentricity;
    Range inclinationDeg;

    // --- approach filters (all on the same row) -----------------------------
    Range dateJd;
    Range distanceAU;
    Range velocityKms;                       // v_rel
    TriState grazing = TriState::Any;        // nominal distance below one Earth radius

    // --- output --------------------------------------------------------------
    SortField     sortBy = SortField::None;
    SortDirection direction = SortDirection::Ascending;
    std::size_t   topK = 0;                  // 0 = every match
};

// Empty when the query is valid; otherwise one human-readable message per
// problem (a lower bound above its upper bound, a bound that is not a number).
std::vector<std::string> validate(const Query& query);

bool hasObjectFilter(const Query& query);
bool hasApproachFilter(const Query& query);

// One-line description for EXPLAIN and logs, e.g.
//   "pha=yes | diameter [0.14, -] km (measured or H-estimate) | date 2030-01-01..2040-01-01 | sort dist asc | top 10"
std::string describe(const Query& query);

} // namespace neo
