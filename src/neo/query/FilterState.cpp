#include "neo/query/FilterState.h"

#include "neo/model/JulianDate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace neo {

double distanceToAu(double value, DistanceUnit unit) {
    return unit == DistanceUnit::LunarDistance ? value * kLunarDistanceAU : value;
}

const char* toString(DistanceUnit unit) { return unit == DistanceUnit::LunarDistance ? "LD" : "AU"; }

void changeDistanceUnit(NeoFilterState& state, DistanceUnit unit) {
    if (unit == state.distanceUnit) {
        return;
    }
    const double au = distanceToAu(static_cast<double>(state.maxDistance), state.distanceUnit);
    state.maxDistance = static_cast<float>(unit == DistanceUnit::LunarDistance ? au / kLunarDistanceAU : au);
    state.distanceUnit = unit;
}

namespace {

// Trims spaces so " 2030-01-01 " is accepted.
std::string trimmed(const char* text) {
    std::string s(text);
    const std::size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) {
        return std::string();
    }
    const std::size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

// Returns false (and an error) for non-empty text that is not a date.
bool parseDate(const char* text, const char* name, bool endOfDay, std::optional<double>& out,
               std::vector<std::string>& errors) {
    const std::string s = trimmed(text);
    if (s.empty()) {
        return true; // open on this side
    }
    double jd = 0.0;
    if (!julianDateFromIsoDate(s, jd)) {
        errors.push_back(std::string(name) + ": '" + s + "' is not a date (expected YYYY-MM-DD)");
        return false;
    }
    // "to" means the end of that day, so a window 2030-01-01 .. 2030-01-01 is one whole day.
    out = endOfDay ? jd + 1.0 - 1e-6 : jd;
    return true;
}

bool finite(float v) { return std::isfinite(v); }

} // namespace

FilterParse toQuery(const NeoFilterState& s) {
    FilterParse out;
    Query& q = out.query;

    std::optional<double> from;
    std::optional<double> to;
    parseDate(s.dateFrom, "date from", false, from, out.errors);
    parseDate(s.dateTo, "date to", true, to, out.errors);
    q.dateJd.lo = from;
    q.dateJd.hi = to;

    if (!finite(s.maxDistance) || s.maxDistance < 0.0f) {
        out.errors.push_back("maximum distance must be a number of zero or more");
    } else if (s.maxDistance > 0.0f) {
        q.distanceAU = Range::atMost(distanceToAu(static_cast<double>(s.maxDistance), s.distanceUnit));
    }

    auto metres = [&](float v, const char* name, std::optional<double>& dst) {
        if (!finite(v) || v < 0.0f) {
            out.errors.push_back(std::string(name) + " must be a number of zero or more");
        } else if (v > 0.0f) {
            dst = static_cast<double>(v) / 1000.0; // m -> km
        }
    };
    metres(s.minDiameterM, "minimum diameter", q.diameterKm.lo);
    metres(s.maxDiameterM, "maximum diameter", q.diameterKm.hi);
    q.diameterMode = s.diameterMode;

    auto speed = [&](float v, const char* name, std::optional<double>& dst) {
        if (!finite(v) || v < 0.0f) {
            out.errors.push_back(std::string(name) + " must be a number of zero or more");
        } else if (v > 0.0f) {
            dst = static_cast<double>(v);
        }
    };
    speed(s.minVelocity, "minimum velocity", q.velocityKms.lo);
    speed(s.maxVelocity, "maximum velocity", q.velocityKms.hi);

    if (s.phaOnly) {
        q.pha = TriState::Yes;
    }
    if (s.grazingOnly) {
        q.grazing = TriState::Yes;
    }
    q.sortBy = s.sortBy;
    q.direction = s.direction;
    q.topK = static_cast<std::size_t>(std::clamp(s.topK, 1, kMaxTopK));

    // lo > hi and the like, in the query's own words.
    for (const std::string& e : validate(q)) {
        out.errors.push_back(e);
    }
    return out;
}

} // namespace neo
