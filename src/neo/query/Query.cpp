#include "neo/query/Query.h"

#include "neo/model/JulianDate.h"

#include <cmath>
#include <cstdio>

namespace neo {

const char* toString(TriState value) {
    switch (value) {
    case TriState::Any: return "any";
    case TriState::Yes: return "yes";
    case TriState::No:  return "no";
    }
    return "?";
}

const char* toString(DiameterMode mode) {
    switch (mode) {
    case DiameterMode::MeasuredOrEstimated: return "measured or H-estimate";
    case DiameterMode::MeasuredOnly:        return "measured only";
    case DiameterMode::IncludeUnknown:      return "measured or H-estimate, unknown included";
    }
    return "?";
}

const char* toString(SortField field) {
    switch (field) {
    case SortField::None:              return "index";
    case SortField::Designation:       return "designation";
    case SortField::Diameter:          return "diameter";
    case SortField::AbsoluteMagnitude: return "H";
    case SortField::Moid:              return "moid";
    case SortField::SemiMajorAxis:     return "a";
    case SortField::Eccentricity:      return "e";
    case SortField::Inclination:       return "i";
    case SortField::Date:              return "date";
    case SortField::Distance:          return "dist";
    case SortField::Velocity:          return "v_rel";
    }
    return "?";
}

bool isApproachSort(SortField field) {
    return field == SortField::Date || field == SortField::Distance || field == SortField::Velocity;
}

namespace {

std::string g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

void checkRange(const char* name, const Range& r, std::vector<std::string>& errors) {
    if (r.lo && std::isnan(*r.lo)) {
        errors.push_back(std::string(name) + ": the lower bound is not a number");
    }
    if (r.hi && std::isnan(*r.hi)) {
        errors.push_back(std::string(name) + ": the upper bound is not a number");
    }
    if (r.lo && r.hi && !std::isnan(*r.lo) && !std::isnan(*r.hi) && *r.lo > *r.hi) {
        errors.push_back(std::string(name) + ": the lower bound " + g(*r.lo) + " is greater than the upper bound " +
                         g(*r.hi) + " (bounds are inclusive, so lower must be <= upper)");
    }
}

std::string rangeText(const Range& r) {
    return "[" + (r.lo ? g(*r.lo) : std::string("-")) + ", " + (r.hi ? g(*r.hi) : std::string("-")) + "]";
}

std::string dateRangeText(const Range& r) {
    return (r.lo ? formatJulianDay(*r.lo) : std::string("...")) + ".." +
           (r.hi ? formatJulianDay(*r.hi) : std::string("..."));
}

} // namespace

std::vector<std::string> validate(const Query& q) {
    std::vector<std::string> errors;
    checkRange("diameter (km)", q.diameterKm, errors);
    checkRange("absolute magnitude H", q.absoluteMagnitude, errors);
    checkRange("MOID (au)", q.moidAU, errors);
    checkRange("semi-major axis a (au)", q.semiMajorAxisAU, errors);
    checkRange("eccentricity e", q.eccentricity, errors);
    checkRange("inclination i (deg)", q.inclinationDeg, errors);
    checkRange("close-approach date (JD)", q.dateJd, errors);
    checkRange("approach distance (au)", q.distanceAU, errors);
    checkRange("relative velocity (km/s)", q.velocityKms, errors);
    return errors;
}

bool hasObjectFilter(const Query& q) {
    return !q.designation.empty() || !q.namePrefix.empty() || q.kind.has_value() || q.neo != TriState::Any ||
           q.pha != TriState::Any || !q.orbitClasses.empty() || q.diameterKm.active() ||
           q.absoluteMagnitude.active() || q.moidAU.active() || q.semiMajorAxisAU.active() ||
           q.eccentricity.active() || q.inclinationDeg.active();
}

bool hasApproachFilter(const Query& q) {
    return q.dateJd.active() || q.distanceAU.active() || q.velocityKms.active() || q.grazing != TriState::Any;
}

std::string describe(const Query& q) {
    std::vector<std::string> parts;
    if (!q.designation.empty()) parts.push_back("designation=" + q.designation);
    if (!q.namePrefix.empty()) parts.push_back("name~" + q.namePrefix + "*");
    if (q.kind) parts.push_back(std::string("kind=") + toString(*q.kind));
    if (q.neo != TriState::Any) parts.push_back(std::string("neo=") + toString(q.neo));
    if (q.pha != TriState::Any) parts.push_back(std::string("pha=") + toString(q.pha));
    if (!q.orbitClasses.empty()) {
        std::string list;
        for (const std::string& c : q.orbitClasses) {
            list += (list.empty() ? "" : ",") + c;
        }
        parts.push_back("class in {" + list + "}");
    }
    if (q.diameterKm.active()) {
        parts.push_back("diameter " + rangeText(q.diameterKm) + " km (" + toString(q.diameterMode) + ")");
    }
    if (q.absoluteMagnitude.active()) parts.push_back("H " + rangeText(q.absoluteMagnitude));
    if (q.moidAU.active()) parts.push_back("moid " + rangeText(q.moidAU) + " au");
    if (q.semiMajorAxisAU.active()) parts.push_back("a " + rangeText(q.semiMajorAxisAU) + " au");
    if (q.eccentricity.active()) parts.push_back("e " + rangeText(q.eccentricity));
    if (q.inclinationDeg.active()) parts.push_back("i " + rangeText(q.inclinationDeg) + " deg");
    if (q.dateJd.active()) parts.push_back("date " + dateRangeText(q.dateJd));
    if (q.distanceAU.active()) parts.push_back("dist " + rangeText(q.distanceAU) + " au");
    if (q.velocityKms.active()) parts.push_back("v_rel " + rangeText(q.velocityKms) + " km/s");
    if (q.grazing != TriState::Any) parts.push_back(std::string("grazing=") + toString(q.grazing));
    if (q.sortBy != SortField::None) {
        parts.push_back(std::string("sort ") + toString(q.sortBy) +
                        (q.direction == SortDirection::Ascending ? " asc" : " desc"));
    }
    if (q.topK > 0) parts.push_back("top " + std::to_string(q.topK));
    if (parts.empty()) return "(no filters)";
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += (i == 0 ? "" : " | ") + parts[i];
    }
    return out;
}

} // namespace neo
