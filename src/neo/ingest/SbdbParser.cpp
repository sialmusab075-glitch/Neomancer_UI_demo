#include "neo/ingest/SbdbParser.h"

#include "neo/ingest/detail/JsonTable.h"

#include <cstdio>

namespace neo {

const char* const kSbdbFields =
    "spkid,full_name,pdes,name,kind,neo,pha,class,orbit_id,epoch,e,a,q,i,om,w,ma,n,per,moid,"
    "condition_code,H,diameter,diameter_sigma,albedo,rot_per";

namespace {

// Column indices resolved once per response, because the API decides the order.
struct Columns {
    int spkid, fullName, pdes, name, kind, neo, pha, orbitClass;
    int orbitId, epoch, e, a, q, i, om, w, ma, n, per, moid, conditionCode;
    int h, diameter, diameterSigma, albedo, rotPer;

    explicit Columns(const detail::JsonTable& t)
        : spkid(t.column("spkid")), fullName(t.column("full_name")), pdes(t.column("pdes")),
          name(t.column("name")), kind(t.column("kind")), neo(t.column("neo")), pha(t.column("pha")),
          orbitClass(t.column("class")), orbitId(t.column("orbit_id")), epoch(t.column("epoch")),
          e(t.column("e")), a(t.column("a")), q(t.column("q")), i(t.column("i")), om(t.column("om")),
          w(t.column("w")), ma(t.column("ma")), n(t.column("n")), per(t.column("per")),
          moid(t.column("moid")), conditionCode(t.column("condition_code")), h(t.column("H")),
          diameter(t.column("diameter")), diameterSigma(t.column("diameter_sigma")),
          albedo(t.column("albedo")), rotPer(t.column("rot_per")) {}
};

} // namespace

ParseStatus parseSbdbObjects(const std::string& json, std::vector<Asteroid>& out, ValidationReport& report,
                             const SbdbParseOptions& options) {
    detail::JsonTable table;
    const ParseStatus status =
        table.parse(json, options.expectedVersion.c_str(), "SBDB query", kSbdbDocUrl, report);
    if (!status) {
        return status;
    }
    if (table.rowCount() == 0) {
        return ParseStatus::success();
    }

    const Columns col(table);
    if (col.pdes < 0) {
        return ParseStatus::failure("SBDB query: response has no 'pdes' column, which is the canonical key");
    }

    out.reserve(out.size() + table.rowCount());
    for (std::size_t r = 0; r < table.rowCount(); ++r) {
        const nlohmann::json& row = table.row(r);
        const std::optional<std::string> pdes = detail::JsonTable::text(row, col.pdes);
        if (!pdes || pdes->empty()) {
            report.noteRejected(r, "missing primary designation (pdes)");
            continue;
        }

        Asteroid a;
        a.pdes = *pdes;
        a.spkid = detail::JsonTable::text(row, col.spkid).value_or(std::string());
        a.name = detail::JsonTable::text(row, col.name).value_or(std::string());
        a.fullName = detail::JsonTable::text(row, col.fullName).value_or(std::string());

        const std::string kind = detail::JsonTable::text(row, col.kind).value_or(std::string());
        a.classification.kind = objectKindFromSbdb(kind);
        a.classification.numbered = kind.size() > 1 && kind[1] == 'n';
        a.classification.isNEO = detail::JsonTable::yesNo(row, col.neo);
        a.classification.isPHA = detail::JsonTable::yesNo(row, col.pha);
        a.classification.orbitClass = detail::JsonTable::text(row, col.orbitClass).value_or(std::string());

        // Orbit: everything the propagator needs must be present and numeric.
        // Note a < 0 for hyperbolic objects and per is null for them, so
        // neither is range-checked here.
        const std::optional<double> epoch = detail::JsonTable::number(row, col.epoch);
        const std::optional<double> e = detail::JsonTable::number(row, col.e);
        const std::optional<double> sma = detail::JsonTable::number(row, col.a);
        const std::optional<double> q = detail::JsonTable::number(row, col.q);
        const std::optional<double> inc = detail::JsonTable::number(row, col.i);
        const std::optional<double> om = detail::JsonTable::number(row, col.om);
        const std::optional<double> w = detail::JsonTable::number(row, col.w);
        const std::optional<double> ma = detail::JsonTable::number(row, col.ma);
        const std::optional<double> n = detail::JsonTable::number(row, col.n);
        const bool haveOrbit = epoch && e && sma && q && inc && om && w && ma && n;
        if (options.requireOrbit && !haveOrbit) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s: incomplete orbit (epoch/e/a/q/i/om/w/ma/n)", a.pdes.c_str());
            report.noteRejected(r, buf);
            continue;
        }
        if (haveOrbit) {
            if (*e < 0.0) {
                report.noteRejected(r, a.pdes + ": negative eccentricity");
                continue;
            }
            a.orbital.epochJdTdb = *epoch;
            a.orbital.eccentricity = *e;
            a.orbital.semiMajorAxisAU = *sma;
            a.orbital.perihelionAU = *q;
            a.orbital.inclinationDeg = *inc;
            a.orbital.ascendingNodeDeg = *om;
            a.orbital.argPerihelionDeg = *w;
            a.orbital.meanAnomalyDeg = *ma;
            a.orbital.meanMotionDegPerDay = *n;
        }
        a.orbital.orbitId = detail::JsonTable::text(row, col.orbitId).value_or(std::string());
        a.orbital.periodDays = detail::JsonTable::number(row, col.per);
        a.orbital.moidAU = detail::JsonTable::number(row, col.moid);
        a.orbital.conditionCode = detail::JsonTable::integer(row, col.conditionCode);

        a.physical.absoluteMagnitudeH = detail::JsonTable::number(row, col.h);
        a.physical.diameterKm = detail::JsonTable::number(row, col.diameter);
        a.physical.diameterSigmaKm = detail::JsonTable::number(row, col.diameterSigma);
        a.physical.albedo = detail::JsonTable::number(row, col.albedo);
        a.physical.rotationPeriodHours = detail::JsonTable::number(row, col.rotPer);

        out.push_back(std::move(a));
        ++report.rowsAccepted;
    }
    return ParseStatus::success();
}

} // namespace neo
