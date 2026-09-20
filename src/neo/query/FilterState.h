#pragma once

#include "neo/query/Query.h"

#include <string>
#include <vector>

namespace neo {

// The NEO FILTER panel's form, as plain data with fixed-size text buffers (Dear
// ImGui edits those in place), and its conversion to a Query. Kept out of the HUD
// code so the unit handling and the "0 = no limit" rule can be tested without a
// window. The HUD never builds a Query any other way.

enum class DistanceUnit : std::uint8_t { LunarDistance, Au };

struct NeoFilterState {
    // Date window, "YYYY-MM-DD". Empty = open on that side. `to` includes the whole day.
    char dateFrom[16] = "";
    char dateTo[16] = "";

    // Maximum approach distance; 0 = no limit.
    float        maxDistance = 10.0f;
    DistanceUnit distanceUnit = DistanceUnit::LunarDistance;

    // Diameter in METRES (asteroids worth filtering are tens to hundreds of
    // metres); 0 = no limit on that side.
    float        minDiameterM = 0.0f;
    float        maxDiameterM = 0.0f;
    DiameterMode diameterMode = DiameterMode::MeasuredOrEstimated;

    // Relative velocity in km/s; 0 = no limit on that side.
    float minVelocity = 0.0f;
    float maxVelocity = 0.0f;

    bool phaOnly = false;
    bool grazingOnly = false;

    SortField     sortBy = SortField::Distance;
    SortDirection direction = SortDirection::Ascending;
    int           topK = 50; // 1 .. kMaxTopK
};

constexpr int kMaxTopK = 1000;

struct FilterParse {
    Query                    query;
    std::vector<std::string> errors; // empty when the form is valid and the query can be run
};

// Builds the Query from the form. Reports text that is not a date, a bound that
// is not a number, and every lo > hi the query's own validation finds, so the
// panel can show them next to the RUN button.
FilterParse toQuery(const NeoFilterState& state);

// Conversions the form uses (and the tests check).
double distanceToAu(double value, DistanceUnit unit);
const char* toString(DistanceUnit unit);

// Switches the unit of the maximum-distance field and converts its value, so the same
// physical distance stays in the box (10 LD becomes 0.0257 AU, not "10 AU"). 0 (no limit) stays 0.
void changeDistanceUnit(NeoFilterState& state, DistanceUnit unit);

} // namespace neo
