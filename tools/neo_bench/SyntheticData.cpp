#include "SyntheticData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace neo {
namespace bench {

namespace {

// 1950-01-01 and 2150-01-01, the default CAD window of neo_ingest.
constexpr double kWindowStartJd = 2433282.5;
constexpr double kWindowEndJd = 2506331.5;

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 6364136223846793005ull + 1442695040888963407ull) {
        for (int i = 0; i < 4; ++i) {
            next();
        }
    }
    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n); }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double lo, double hi) { return lo + (hi - lo) * unit(); }
    double normal(double mean, double sd) {
        const double u1 = std::max(unit(), 1e-12);
        const double u2 = unit();
        return mean + sd * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
    double clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

private:
    std::uint64_t state_;
};

std::string spkidFor(std::size_t index) { return std::to_string(20000000 + index); }

// Distributions, with the SBDB population they imitate in brackets:
//   H            normal(22.5, 2.3) clipped to 10..30.5      [NEO H peaks near 22-23]
//   q, a, e      q uniform 0.2..1.3 au (NEO: q < 1.3), a lognormal around 1.9 au, e = 1 - q/a
//   i            exponential, mean 12 deg                   [most NEOs are low inclination]
//   MOID         0.3 * u^2 au                               [piles up near 0]
//   diameter     measured for 3 % (from H and a random albedo), the rest known only through H
//   PHA          6 %, class APO 42 / AMO 40 / ATE 14 / IEO 4 %
//   approaches   46 % of objects have at least one, 1 + geometric, mean ~2.2 each [real: 19,655
//                of 42,666 objects, 42,819 approaches]; distance 0.05 * sqrt(u) au (flux through a
//                disc, so more far passes than near ones); v_rel lognormal, median 9.5 km/s.
Asteroid makeParametricObject(Rng& rng, std::size_t index) {
    Asteroid a;
    a.pdes = syntheticDesignation(index);
    a.spkid = spkidFor(index);
    if (index % 50 == 0) {
        a.name = "Syn" + std::to_string(index);
    }
    a.fullName = "   " + a.pdes + (a.name.empty() ? "" : " " + a.name);

    a.classification.kind = ObjectKind::Asteroid;
    a.classification.numbered = (index % 10 == 0);
    a.classification.isNEO = true;
    a.classification.isPHA = rng.unit() < 0.06;
    const double c = rng.unit();
    a.classification.orbitClass = c < 0.42 ? "APO" : c < 0.82 ? "AMO" : c < 0.96 ? "ATE" : "IEO";

    OrbitalProperties& o = a.orbital;
    o.orbitId = "1";
    o.epochJdTdb = 2461200.5;
    const double q = rng.uniform(0.2, 1.3);
    double semiMajor = rng.clamp(std::exp(rng.normal(std::log(1.9), 0.35)), 0.6, 5.0);
    if (semiMajor <= q * 1.02) {
        semiMajor = q * (1.02 + 3.0 * rng.unit());
    }
    o.semiMajorAxisAU = semiMajor;
    o.perihelionAU = q;
    o.eccentricity = 1.0 - q / semiMajor;
    o.inclinationDeg = rng.clamp(-12.0 * std::log(std::max(rng.unit(), 1e-9)), 0.05, 60.0);
    o.ascendingNodeDeg = rng.uniform(0.0, 360.0);
    o.argPerihelionDeg = rng.uniform(0.0, 360.0);
    o.meanAnomalyDeg = rng.uniform(0.0, 360.0);
    const double a15 = std::pow(semiMajor, 1.5);
    o.meanMotionDegPerDay = 0.9856076686 / a15;
    o.periodDays = 365.256898 * a15;
    if (rng.unit() < 0.997) {
        const double u = rng.unit();
        o.moidAU = 0.3 * u * u;
    }
    o.conditionCode = static_cast<int>(rng.below(10));

    PhysicalProperties& p = a.physical;
    if (rng.unit() < 0.995) {
        p.absoluteMagnitudeH = rng.clamp(rng.normal(22.5, 2.3), 10.0, 30.5);
    }
    if (p.absoluteMagnitudeH && rng.unit() < 0.03) {
        const double albedo = rng.uniform(0.03, 0.5);
        p.albedo = albedo;
        const double d = diameterFromMagnitude(*p.absoluteMagnitudeH, albedo);
        p.diameterKm = d;
        p.diameterSigmaKm = d * 0.1;
    }
    if (rng.unit() < 0.08) {
        p.rotationPeriodHours = std::exp(rng.normal(std::log(6.0), 0.8));
    }
    return a;
}

CloseApproach makeParametricApproach(Rng& rng, std::uint32_t object, const Asteroid& owner) {
    CloseApproach c;
    c.objectIndex = object;
    c.jdTdb = rng.uniform(kWindowStartJd, kWindowEndJd);
    c.distanceAU = 0.05 * std::sqrt(rng.unit());
    const double lo = rng.unit();
    const double hi = rng.unit();
    c.distanceMinAU = c.distanceAU * (1.0 - 0.5 * lo * lo);
    c.distanceMaxAU = c.distanceAU * (1.0 + 1.5 * hi * hi);
    c.relVelocityKms = rng.clamp(std::exp(rng.normal(std::log(9.5), 0.55)), 0.5, 70.0);
    if (rng.unit() < 0.999) {
        c.vInfinityKms = c.relVelocityKms * 0.995;
    }
    c.absoluteMagnitudeH = owner.physical.absoluteMagnitudeH;
    c.diameterKm = owner.physical.diameterKm;
    c.diameterSigmaKm = owner.physical.diameterSigmaKm;
    return c;
}

} // namespace

std::string syntheticDesignation(std::size_t index) {
    const std::size_t year = 1950 + index % 75;
    const std::size_t rest = index / 75;
    const char first = static_cast<char>('A' + rest % 25);
    const char second = static_cast<char>('A' + (rest / 25) % 25);
    const std::size_t serial = rest / 625;
    std::string out = std::to_string(year) + " ";
    out += first;
    out += second;
    if (serial > 0) {
        out += std::to_string(serial);
    }
    return out;
}

Dataset makeSyntheticDataset(const SyntheticOptions& options, SyntheticInfo* info) {
    Rng rng(options.seed);
    const Dataset* real = options.resampleFrom;
    const bool resample = real != nullptr && real->objectCount() > 0;

    std::vector<Asteroid> objects;
    objects.reserve(options.objects);
    std::vector<CloseApproach> approaches;
    approaches.reserve(options.objects / 2 * 2 + 16);

    for (std::size_t i = 0; i < options.objects; ++i) {
        const std::uint32_t object = static_cast<std::uint32_t>(i);
        if (resample) {
            const std::uint32_t source = rng.below(static_cast<std::uint32_t>(real->objectCount()));
            Asteroid a = real->records()[source].object;
            a.pdes = syntheticDesignation(i);
            a.spkid = spkidFor(i);
            a.name = (i % 50 == 0) ? "Syn" + std::to_string(i) : std::string();
            a.fullName = "   " + a.pdes + (a.name.empty() ? "" : " " + a.name);
            for (const CloseApproach& original : real->approachesOf(source)) {
                CloseApproach c = original;
                c.objectIndex = object;
                c.jdTdb = rng.clamp(original.jdTdb + rng.uniform(-1095.0, 1095.0), kWindowStartJd, kWindowEndJd);
                approaches.push_back(c);
            }
            objects.push_back(std::move(a));
        } else {
            Asteroid a = makeParametricObject(rng, i);
            if (rng.unit() < 0.46) {
                int count = 1;
                while (rng.unit() < 0.54 && count < 40) {
                    ++count;
                }
                for (int k = 0; k < count; ++k) {
                    approaches.push_back(makeParametricApproach(rng, object, a));
                }
            }
            objects.push_back(std::move(a));
        }
    }

    Dataset dataset;
    dataset.setObjects(std::move(objects));
    dataset.setApproaches(std::move(approaches));
    if (info != nullptr) {
        info->kind = resample ? "synthetic-resampled" : "synthetic-parametric";
        info->objects = dataset.objectCount();
        info->approaches = dataset.approachCount();
    }
    return dataset;
}

Dataset prefixOfDataset(const Dataset& source, std::size_t objects) {
    const std::size_t count = std::min(objects, source.objectCount());
    std::vector<Asteroid> kept;
    kept.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
        kept.push_back(source.records()[r].object);
    }
    std::vector<CloseApproach> approaches;
    for (std::size_t r = 0; r < count; ++r) {
        for (const CloseApproach& c : source.approachesOf(static_cast<std::uint32_t>(r))) {
            approaches.push_back(c);
        }
    }
    Dataset dataset;
    dataset.setObjects(std::move(kept));
    dataset.setApproaches(std::move(approaches));
    return dataset;
}

namespace {

void appendNumber(std::string& out, const std::optional<double>& value) {
    if (!value) {
        out += "null";
        return;
    }
    char buf[48];
    std::snprintf(buf, sizeof buf, "\"%.15g\"", *value);
    out += buf;
}

void appendNumber(std::string& out, double value) { appendNumber(out, std::optional<double>(value)); }

void appendString(std::string& out, const std::string& value) {
    out += '"';
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
}

void appendFlag(std::string& out, const Flag& flag) {
    out += flag ? (*flag ? "\"Y\"" : "\"N\"") : "null";
}

} // namespace

std::string makeSbdbPageJson(const Dataset& dataset, std::size_t firstRecord, std::size_t count) {
    const std::size_t last = std::min(dataset.objectCount(), firstRecord + count);
    std::string out =
        "{\"signature\":{\"version\":\"1.0\",\"source\":\"NASA/JPL SBDB (Small-Body DataBase) Query API\"},"
        "\"fields\":[\"spkid\",\"full_name\",\"pdes\",\"name\",\"kind\",\"neo\",\"pha\",\"class\",\"orbit_id\","
        "\"epoch\",\"e\",\"a\",\"q\",\"i\",\"om\",\"w\",\"ma\",\"n\",\"per\",\"moid\",\"condition_code\",\"H\","
        "\"diameter\",\"diameter_sigma\",\"albedo\",\"rot_per\"],\"data\":[";
    for (std::size_t r = firstRecord; r < last; ++r) {
        const Asteroid& a = dataset.records()[r].object;
        if (r != firstRecord) {
            out += ',';
        }
        out += '[';
        out += a.spkid.empty() ? "null" : a.spkid;
        out += ',';
        appendString(out, a.fullName);
        out += ',';
        appendString(out, a.pdes);
        out += ',';
        if (a.name.empty()) {
            out += "null";
        } else {
            appendString(out, a.name);
        }
        out += a.classification.numbered ? ",\"an\"," : ",\"au\",";
        appendFlag(out, a.classification.isNEO);
        out += ',';
        appendFlag(out, a.classification.isPHA);
        out += ',';
        appendString(out, a.classification.orbitClass);
        out += ',';
        appendString(out, a.orbital.orbitId);
        const OrbitalProperties& o = a.orbital;
        const double fields[] = {o.epochJdTdb, o.eccentricity, o.semiMajorAxisAU, o.perihelionAU, o.inclinationDeg,
                                 o.ascendingNodeDeg, o.argPerihelionDeg, o.meanAnomalyDeg, o.meanMotionDegPerDay};
        for (const double v : fields) {
            out += ',';
            appendNumber(out, v);
        }
        out += ',';
        appendNumber(out, o.periodDays);
        out += ',';
        appendNumber(out, o.moidAU);
        out += ',';
        if (o.conditionCode) {
            out += '"' + std::to_string(*o.conditionCode) + '"';
        } else {
            out += "null";
        }
        const PhysicalProperties& p = a.physical;
        for (const std::optional<double>& v :
             {p.absoluteMagnitudeH, p.diameterKm, p.diameterSigmaKm, p.albedo, p.rotationPeriodHours}) {
            out += ',';
            appendNumber(out, v);
        }
        out += ']';
    }
    out += "],\"count\":" + std::to_string(last - firstRecord) + "}";
    return out;
}

} // namespace bench
} // namespace neo
