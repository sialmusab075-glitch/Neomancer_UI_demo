#pragma once

#include <vector>

namespace sim {
class SolarSystem;
}

namespace hud {

struct HudState;
struct HudEvents;

// Heliocentric distance of the selected body over the last N sim-days, drawn as
// a sparkline, plus every planet's orbit phase (bar gauges or a flat histogram,
// depending on the theme). Cleared when the selection changes or time jumps.
// Buffers are reserved up front, so drawing and recording never allocate.
class DistanceHistory {
public:
    static constexpr int kMaxSamples = 720;
    static constexpr int kCapacity = 1024; // > kMaxSamples + the one-sample overshoot of record()

    DistanceHistory();

    void reset();
    // Records a sample if sim time moved at least windowDays / kMaxSamples since the last one.
    void record(double t_days, double r_AU, double windowDays);

    void draw(const sim::SolarSystem& system, HudState& state, HudEvents& events, double now_days,
              bool timeRunsBackwards) const;

private:
    std::vector<double> t_;          // sim time of each sample (days)
    std::vector<double> r_;          // distance (AU)
    mutable std::vector<double> xs_; // scratch: sample time relative to now (reused every frame)
};

} // namespace hud
