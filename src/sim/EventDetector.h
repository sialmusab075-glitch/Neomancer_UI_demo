#pragma once

#include "sim/Body.h"

#include <vector>

namespace sim {

class SolarSystem;

enum class EventType {
    Perihelion,  // dr/dt crosses 0 going from - to + (in sim-time order)
    Aphelion,    // dr/dt crosses 0 going from + to -
    Opposition,  // outer planet: angle Sun->Earth vs Sun->planet crosses 0 deg
    Conjunction, // outer planet: that angle crosses 180 deg (planet behind the Sun)
};

struct SimEvent {
    EventType type;
    int       body;     // index into SolarSystem::bodies()
    double    t_days;   // refined event time, days since J2000.0
    double    value_AU; // perihelion/aphelion: distance to parent; opposition/conjunction: distance to Earth
};

const char* eventName(EventType type);

// Radial velocity dr/dt of a parent-relative state, km/s.
double radialVelocity(const OrbitState& s);

// Detects orbital events by sign changes between consecutive frames, then
// refines each event time by bisection on the analytic orbit (Kepler
// propagation can be evaluated at any t), so reported times and distances do
// not depend on the frame rate or time scale. Works with negative time scales:
// the classification uses sim-time order, not frame order.
class EventDetector {
public:
    // Forget the previous sample (call after a time jump such as reset-to-epoch).
    void reset();

    // Call once per frame after SolarSystem::update(). Appends events that
    // happened between the previous call and now, in the direction time moved.
    void update(const SolarSystem& system, std::vector<SimEvent>& out);

private:
    bool have_ = false;
    double prevT_ = 0.0;
    std::vector<double> prevRdot_;  // per body
    std::vector<double> prevCross_; // per body: z of (Earth x planet), heliocentric
};

} // namespace sim
