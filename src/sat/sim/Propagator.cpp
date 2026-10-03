#include "sat/sim/Propagator.h"

#include "sat/parse/TleParser.h"

#include <SGP4.h>

#include <cmath>
#include <limits>

namespace sat {

struct Propagator::Impl {
    elsetrec    satrec;
    ElementSet  elements;
    bool        ok = false;
};

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
// Days since 1949-12-31 00:00 UT, the epoch Vallado's code counts from (JD 2433281.5).
constexpr double kSgp4EpochJd = 2433281.5;
// The same constants python-sgp4 divides by (derived in Vallado's twoline2rv): TLE and OMM publish
// ndot in rev/day^2 and nddot in rev/day^3, SGP4 wants rad/min^2 and rad/min^3.
constexpr double kNdotUnits = 1036800.0 / kPi;
constexpr double kNddotUnits = 2985984000.0 / 2.0 / kPi;

void setNaN(Propagator::State& s) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (int i = 0; i < 3; ++i) {
        s.r[i] = nan;
        s.v[i] = nan;
    }
}

} // namespace

Propagator::Propagator() = default;
Propagator::~Propagator() = default;
Propagator::Propagator(Propagator&&) noexcept = default;
Propagator& Propagator::operator=(Propagator&&) noexcept = default;

bool Propagator::init(const ElementSet& e, std::string& error, int* sgp4Error) {
    impl_.reset();
    if (sgp4Error != nullptr) {
        *sgp4Error = 0;
    }
    std::string reason;
    if (!validateElementSet(e, reason)) {
        error = reason;
        return false;
    }
    auto impl = std::make_unique<Impl>();
    impl->satrec = elsetrec();
    impl->elements = e;

    // sgp4init copies this with strcpy into a 6-byte field: never pass more than five characters.
    const std::string number = formatCatalogNumber5(e.noradId);
    const double epoch = e.epochJdWhole + e.epochJdFraction - kSgp4EpochJd;
    const double noKozai = e.meanMotionRevPerDay / 720.0 * kPi; // rev/day -> rad/min
    const bool ok = SGP4Funcs::sgp4init(wgs72, 'i', number.c_str(), epoch, e.bstar, e.meanMotionDot / kNdotUnits,
                                        e.meanMotionDdot / kNddotUnits, e.eccentricity, e.argPerigeeDeg * kDegToRad,
                                        e.inclinationDeg * kDegToRad, e.meanAnomalyDeg * kDegToRad, noKozai,
                                        e.raanDeg * kDegToRad, impl->satrec);
    if (!ok || impl->satrec.error != 0) {
        const int code = impl->satrec.error != 0 ? impl->satrec.error : 6;
        if (sgp4Error != nullptr) {
            *sgp4Error = code;
        }
        error = std::string("SGP4 rejected the elements: ") + errorMessage(code);
        return false;
    }
    impl->ok = true;
    impl_ = std::move(impl);
    return true;
}

bool Propagator::valid() const { return impl_ != nullptr && impl_->ok; }

const ElementSet& Propagator::elements() const {
    static const ElementSet empty;
    return impl_ ? impl_->elements : empty;
}

int Propagator::propagateMinutes(double minutes, State& out) {
    if (!valid()) {
        setNaN(out);
        return kNotInitialised;
    }
    if (!SGP4Funcs::sgp4(impl_->satrec, minutes, out.r, out.v)) {
        setNaN(out);
        return impl_->satrec.error != 0 ? impl_->satrec.error : 6;
    }
    return 0;
}

int Propagator::propagateJd(double jdWhole, double jdFraction, State& out) {
    if (!valid()) {
        setNaN(out);
        return kNotInitialised;
    }
    const ElementSet& e = impl_->elements;
    const double minutes = ((jdWhole - e.epochJdWhole) + (jdFraction - e.epochJdFraction)) * 1440.0;
    return propagateMinutes(minutes, out);
}

const char* Propagator::errorMessage(int code) {
    switch (code) {
    case 0: return "no error";
    case 1: return "mean eccentricity is outside 0 <= e < 1";
    case 2: return "mean motion is not positive";
    case 3: return "perturbed eccentricity is outside 0 <= e <= 1";
    case 4: return "semi-latus rectum is negative";
    case 5: return "epoch elements are sub-orbital";
    case 6: return "the satellite has decayed";
    case kNotInitialised: return "the propagator was not initialised";
    default: return "unknown SGP4 error";
    }
}

} // namespace sat
