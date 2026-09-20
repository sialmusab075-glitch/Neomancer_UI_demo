#include "neo/query/IndexSet.h"

#include <chrono>
#include <cmath>
#include <optional>

namespace neo {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint8_t flagCode(const std::optional<bool>& flag) {
    if (!flag) {
        return 0;
    }
    return *flag ? 1 : 2;
}

std::size_t columnBytes(const std::vector<double>& v) { return v.capacity() * sizeof(double); }

// A key function over a column: NaN means unknown and keeps the row out of the view.
struct ColumnKey {
    const std::vector<double>* column;
    std::optional<double> operator()(std::uint32_t i) const {
        const double v = (*column)[i];
        if (std::isnan(v)) {
            return std::nullopt;
        }
        return v;
    }
};

} // namespace

std::uint16_t IndexSet::classIdOf(const std::string& code) const {
    for (std::size_t i = 0; i < classNames.size(); ++i) {
        if (classNames[i] == code) {
            return static_cast<std::uint16_t>(i);
        }
    }
    return kNoClass;
}

void IndexSet::build(const Dataset& ds) {
    dataset = &ds;
    report.clear();
    const Clock::time_point buildStart = Clock::now();

    const std::size_t nObj = ds.records().size();
    const std::size_t nApp = ds.approaches().size();

    auto record = [this](const char* name, Clock::time_point start, std::size_t bytes, std::size_t entries) {
        IndexBuildInfo info;
        info.name = name;
        info.buildMs = msSince(start);
        info.bytes = bytes;
        info.entries = entries;
        report.push_back(info);
    };

    // --- columns -----------------------------------------------------------
    {
        const Clock::time_point t = Clock::now();
        obj = ObjectColumns();
        app = ApproachColumns();
        classNames.clear();
        stats = CategoryStats();

        obj.diameterMeasured.assign(nObj, kUnknownValue);
        obj.diameterBest.assign(nObj, kUnknownValue);
        obj.h.assign(nObj, kUnknownValue);
        obj.moid.assign(nObj, kUnknownValue);
        obj.a.assign(nObj, kUnknownValue);
        obj.e.assign(nObj, kUnknownValue);
        obj.i.assign(nObj, kUnknownValue);
        obj.kind.assign(nObj, 0);
        obj.neo.assign(nObj, 0);
        obj.pha.assign(nObj, 0);
        obj.classId.assign(nObj, kNoClass);

        for (std::size_t r = 0; r < nObj; ++r) {
            const Asteroid& o = ds.records()[r].object;
            if (o.physical.diameterKm) {
                obj.diameterMeasured[r] = *o.physical.diameterKm;
            }
            if (const std::optional<double> best = o.physical.bestDiameterKm()) {
                obj.diameterBest[r] = *best;
            }
            if (o.physical.absoluteMagnitudeH) {
                obj.h[r] = *o.physical.absoluteMagnitudeH;
            }
            if (o.orbital.moidAU) {
                obj.moid[r] = *o.orbital.moidAU;
            }
            obj.a[r] = o.orbital.semiMajorAxisAU;
            obj.e[r] = o.orbital.eccentricity;
            obj.i[r] = o.orbital.inclinationDeg;
            obj.kind[r] = static_cast<std::uint8_t>(o.classification.kind);
            obj.neo[r] = flagCode(o.classification.isNEO);
            obj.pha[r] = flagCode(o.classification.isPHA);
            if (!o.classification.orbitClass.empty()) {
                std::uint16_t id = classIdOf(o.classification.orbitClass);
                if (id == kNoClass) {
                    id = static_cast<std::uint16_t>(classNames.size());
                    classNames.push_back(o.classification.orbitClass);
                    stats.classCounts.push_back(0);
                }
                obj.classId[r] = id;
                ++stats.classCounts[id];
            }
            ++stats.kind[obj.kind[r] < 3 ? obj.kind[r] : 2];
            ++stats.neo[obj.neo[r]];
            ++stats.pha[obj.pha[r]];
        }

        app.jd.resize(nApp);
        app.dist.resize(nApp);
        app.vrel.resize(nApp);
        for (std::size_t k = 0; k < nApp; ++k) {
            const CloseApproach& c = ds.approaches()[k];
            app.jd[k] = c.jdTdb;
            app.dist[k] = c.distanceAU;
            app.vrel[k] = c.relVelocityKms;
            if (c.grazingOrImpact()) {
                ++stats.grazing;
            }
        }

        std::size_t bytes = columnBytes(obj.diameterMeasured) + columnBytes(obj.diameterBest) + columnBytes(obj.h) +
                            columnBytes(obj.moid) + columnBytes(obj.a) + columnBytes(obj.e) + columnBytes(obj.i) +
                            obj.kind.capacity() + obj.neo.capacity() + obj.pha.capacity() +
                            obj.classId.capacity() * sizeof(std::uint16_t) + columnBytes(app.jd) +
                            columnBytes(app.dist) + columnBytes(app.vrel);
        record("columns (fields as dense arrays)", t, bytes, nObj + nApp);
    }

    // --- exact lookup ---------------------------------------------------------
    {
        const Clock::time_point t = Clock::now();
        byDesignation = dsa::HashMap<std::string, std::uint32_t>(nObj);
        for (std::uint32_t r = 0; r < nObj; ++r) {
            byDesignation.insert(ds.records()[r].object.pdes, r);
        }
        record("hash map: designation -> record", t, byDesignation.memoryBytes(), byDesignation.size());
    }
    {
        const Clock::time_point t = Clock::now();
        bySpkId = dsa::HashMap<std::string, std::uint32_t>(nObj);
        for (std::uint32_t r = 0; r < nObj; ++r) {
            if (!ds.records()[r].object.spkid.empty()) {
                bySpkId.insert(ds.records()[r].object.spkid, r);
            }
        }
        record("hash map: SPK-ID -> record", t, bySpkId.memoryBytes(), bySpkId.size());
    }
    {
        const Clock::time_point t = Clock::now();
        names.build(ds);
        record("name index (sorted, prefix search)", t, names.memoryBytes(), names.size());
    }

    // --- ordered views ---------------------------------------------------------
    auto view = [&](const char* name, dsa::SortedView& target, const std::vector<double>& column) {
        const Clock::time_point t = Clock::now();
        target = dsa::buildSortedView(column.size(), ColumnKey{&column});
        record(name, t, target.memoryBytes(), target.size());
    };
    view("sorted view: diameter (measured)", diameterMeasured, obj.diameterMeasured);
    view("sorted view: diameter (measured or estimate)", diameterBest, obj.diameterBest);
    view("sorted view: H", h, obj.h);
    view("sorted view: MOID", moid, obj.moid);
    view("sorted view: semi-major axis a", a, obj.a);
    view("sorted view: eccentricity e", e, obj.e);
    view("sorted view: inclination i", i, obj.i);
    view("sorted view: approach distance", dist, app.dist);
    view("sorted view: approach v_rel", vrel, app.vrel);

    {
        const Clock::time_point t = Clock::now();
        dateTree.clear();
        dateTree.reserve(nApp);
        for (std::uint32_t k = 0; k < nApp; ++k) {
            dateTree.insert(app.jd[k], k);
        }
        record("AVL tree: approach date", t, dateTree.memoryBytes(), dateTree.size());
    }

    // --- categorical ------------------------------------------------------------
    {
        const Clock::time_point t = Clock::now();
        sizeMeasured.build(ds, dsa::DiameterPolicy::MeasuredOnly);
        record("size buckets (measured)", t, sizeMeasured.memoryBytes(), sizeMeasured.size());
    }
    {
        const Clock::time_point t = Clock::now();
        sizeBest.build(ds, dsa::DiameterPolicy::MeasuredOrEstimate);
        record("size buckets (measured or estimate)", t, sizeBest.memoryBytes(), sizeBest.size());
    }
    {
        const Clock::time_point t = Clock::now();
        years.build(ds);
        record("year buckets (approaches)", t, years.memoryBytes(), years.size());
    }

    // --- histograms: built from arrays that are already sorted, so no extra sort
    {
        const Clock::time_point t = Clock::now();
        histDiameterMeasured.build(diameterMeasured.keys.data(), diameterMeasured.size(), nObj);
        histDiameterBest.build(diameterBest.keys.data(), diameterBest.size(), nObj);
        histH.build(h.keys.data(), h.size(), nObj);
        histMoid.build(moid.keys.data(), moid.size(), nObj);
        histA.build(a.keys.data(), a.size(), nObj);
        histE.build(e.keys.data(), e.size(), nObj);
        histI.build(i.keys.data(), i.size(), nObj);
        histDist.build(dist.keys.data(), dist.size(), nApp);
        histVrel.build(vrel.keys.data(), vrel.size(), nApp);
        // The date index is a tree; its in-order walk is already ascending.
        std::vector<double> dates;
        dates.reserve(nApp);
        dateTree.inOrder([&dates](double key, std::uint32_t) { dates.push_back(key); });
        histDate.build(dates.data(), dates.size(), nApp);
        const std::size_t bytes = histDiameterMeasured.memoryBytes() + histDiameterBest.memoryBytes() +
                                  histH.memoryBytes() + histMoid.memoryBytes() + histA.memoryBytes() +
                                  histE.memoryBytes() + histI.memoryBytes() + histDate.memoryBytes() +
                                  histDist.memoryBytes() + histVrel.memoryBytes();
        record("equi-depth histograms (10 fields, 64 bins)", t, bytes, 10 * EquiDepthHistogram::kDefaultBins);
    }

    totalBuildMs = msSince(buildStart);
    totalBytes = 0;
    for (const IndexBuildInfo& info : report) {
        totalBytes += info.bytes;
    }
}

} // namespace neo
