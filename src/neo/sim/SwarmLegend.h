#pragma once

#include "neo/model/Dataset.h"

#include <cstdint>

namespace neo {

// What the colour and brightness of a NEOS point mean. One legend, defined here so
// the shader, the HUD's legend bar and the tests agree on the ranges, and so the
// Earth view can adopt the same one.
enum class SwarmLegend : std::uint8_t {
    Distance, // distance from Earth, now: near = bright accent, far = dim
    Pha,      // potentially hazardous = bright accent, the rest dim
    Diameter, // log diameter: 10 m .. 10 km
    Approach, // time until the object's next close approach: soon = bright
};

constexpr int kSwarmLegendCount = 4;

const char* toString(SwarmLegend legend);
// The two ends of the ramp as text, for the legend bar ("0.01 AU", "5 AU").
const char* legendNearText(SwarmLegend legend);
const char* legendFarText(SwarmLegend legend);

// Ramp ranges (the shader uses the same numbers).
constexpr float kLegendDistanceNearAu = 0.01f;  // log ramp: 0.01 .. 5 AU
constexpr float kLegendDistanceFarAu = 5.0f;
constexpr float kLegendDiameterLogMin = -2.0f;  // log10(km): 10 m
constexpr float kLegendDiameterLogMax = 1.0f;   // 10 km
constexpr float kLegendApproachDays = 365.0f;   // linear ramp: 0 .. one year

constexpr float kUnknownLogDiameter = -9.0f;    // "no diameter and no H"
constexpr float kNoApproach = -1.0f;            // "no later approach in the data"

// The per-object part that does not depend on the clock. The days-to-approach
// field is filled in (and refreshed about once per simulated day) by the caller.
struct SwarmAttr {
    float logDiameterKm = kUnknownLogDiameter;
    float pha = 0.0f;
    float daysToApproach = kNoApproach;
};

SwarmAttr makeSwarmAttr(const Asteroid& asteroid);

// Days from `jdNow` (JD TDB) to the object's earliest approach STRICTLY after it, or
// kNoApproach when its last approach is already past.
float daysToNextApproach(const Dataset& dataset, std::uint32_t record, double jdNow);

} // namespace neo
