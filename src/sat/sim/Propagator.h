#pragma once

#include "sat/model/ElementSet.h"

#include <memory>
#include <string>

namespace sat {

// SGP4 for one satellite, on top of Vallado's reference implementation (external/sgp4). Mean
// elements in, TEME position and velocity out, WGS-72 constants, "improved" operation mode: the
// same choices as python-sgp4 and the reference outputs the tests compare against.
//
// A Propagator is NOT thread-safe: the deep-space branch keeps its integrator state in the object
// (results do not depend on call order, only the work does). Give each thread its own.
class Propagator {
public:
    struct State {
        double r[3] = {0.0, 0.0, 0.0}; // km, TEME
        double v[3] = {0.0, 0.0, 0.0}; // km/s, TEME
    };

    static constexpr int kNotInitialised = -1;

    Propagator();
    ~Propagator();
    Propagator(Propagator&&) noexcept;
    Propagator& operator=(Propagator&&) noexcept;
    Propagator(const Propagator&) = delete;
    Propagator& operator=(const Propagator&) = delete;

    // Initialises from the elements. False (with a reason) when SGP4 rejects them; `sgp4Error`, when
    // given, receives SGP4's error code (see propagateMinutes) or 0 when the elements failed our own
    // range checks before SGP4 saw them. SGP4 runs one step at the epoch while initialising, so a
    // satellite that fails at t = 0 fails here.
    bool init(const ElementSet& elements, std::string& error, int* sgp4Error = nullptr);
    bool valid() const;
    const ElementSet& elements() const;

    // Minutes since the element set's epoch. Returns 0 on success, otherwise SGP4's error code
    // (1 mean eccentricity out of range, 2 mean motion not positive, 3 perturbed eccentricity out of
    // range, 4 semi-latus rectum negative, 5 sub-orbital epoch elements, 6 decayed), or
    // kNotInitialised. `out` is set to NaN on error.
    int propagateMinutes(double minutesSinceEpoch, State& out);

    // At a UTC Julian Date given as (whole, fraction), as ElementSet keeps its epoch, so a date a
    // century away loses no precision.
    int propagateJd(double jdWhole, double jdFraction, State& out);

    static const char* errorMessage(int code);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sat
