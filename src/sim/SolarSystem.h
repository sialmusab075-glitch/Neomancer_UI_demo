#pragma once

#include "sim/Body.h"

#include <cstddef>
#include <vector>

namespace sim {

// A set of bodies propagated analytically (two-body Kepler motion about each
// body's parent). Physics runs in real units: AU, km/s, days.
class SolarSystem {
public:
    // tableIndices: rows of the body table to instantiate. Each body's parent
    // must be listed before it (the Sun first).
    explicit SolarSystem(const std::vector<int>& tableIndices);

    // Sun and all eight planets.
    static SolarSystem createFull();

    // Recomputes every body for simulation time t (days since J2000.0).
    void update(double t_days);

    double timeDays() const { return t_days_; }
    const std::vector<Body>& bodies() const { return bodies_; }
    const Body& body(int index) const { return bodies_[static_cast<std::size_t>(index)]; }
    int bodyCount() const { return static_cast<int>(bodies_.size()); }

    // Index into bodies() for a table row, or -1 if that body is not present.
    int indexOfTableRow(int tableIndex) const;

private:
    std::vector<Body> bodies_;
    double t_days_ = 0.0;
};

} // namespace sim
