#include "neo/ingest/CadParser.h"

#include "neo/ingest/detail/JsonTable.h"

namespace neo {

namespace {

struct Columns {
    int des, orbitId, jd, dist, distMin, distMax, vRel, vInf, h, diameter, diameterSigma;

    explicit Columns(const detail::JsonTable& t)
        : des(t.column("des")), orbitId(t.column("orbit_id")), jd(t.column("jd")), dist(t.column("dist")),
          distMin(t.column("dist_min")), distMax(t.column("dist_max")), vRel(t.column("v_rel")),
          vInf(t.column("v_inf")), h(t.column("h")), diameter(t.column("diameter")),
          diameterSigma(t.column("diameter_sigma")) {}
};

} // namespace

ParseStatus parseCadApproaches(const std::string& json, std::vector<ParsedApproach>& out, ValidationReport& report,
                               const CadParseOptions& options) {
    detail::JsonTable table;
    const ParseStatus status =
        table.parse(json, options.expectedVersion.c_str(), "CAD", kCadDocUrl, report);
    if (!status) {
        return status;
    }
    if (table.rowCount() == 0) {
        return ParseStatus::success();
    }

    const Columns col(table);
    if (col.des < 0 || col.jd < 0) {
        return ParseStatus::failure("CAD: response is missing the 'des' or 'jd' column");
    }

    out.reserve(out.size() + table.rowCount());
    for (std::size_t r = 0; r < table.rowCount(); ++r) {
        const nlohmann::json& row = table.row(r);
        const std::optional<std::string> des = detail::JsonTable::text(row, col.des);
        if (!des || des->empty()) {
            report.noteRejected(r, "missing designation (des)");
            continue;
        }
        const std::optional<double> jd = detail::JsonTable::number(row, col.jd);
        if (!jd) {
            report.noteRejected(r, *des + ": missing or non-numeric approach time (jd)");
            continue;
        }
        const std::optional<double> dist = detail::JsonTable::number(row, col.dist);
        if (options.requireDistance && !dist) {
            report.noteRejected(r, *des + ": missing approach distance (dist)");
            continue;
        }
        const std::optional<double> vRel = detail::JsonTable::number(row, col.vRel);

        ParsedApproach parsed;
        parsed.designation = *des;
        parsed.orbitId = detail::JsonTable::text(row, col.orbitId).value_or(std::string());

        CloseApproach& a = parsed.approach;
        a.jdTdb = *jd;
        a.distanceAU = dist.value_or(0.0);
        // The 3-sigma bounds are always present in practice. When a response
        // omits them the nominal distance stands in, so a range query cannot
        // match a spurious 0 AU; the row is flagged and the substitution counted.
        const std::optional<double> distMin = detail::JsonTable::number(row, col.distMin);
        const std::optional<double> distMax = detail::JsonTable::number(row, col.distMax);
        a.distanceMinAU = distMin.value_or(a.distanceAU);
        a.distanceMaxAU = distMax.value_or(a.distanceAU);
        a.distRangeDerived = !distMin || !distMax;
        if (a.distRangeDerived) {
            ++report.derivedDistanceRanges;
        }
        a.relVelocityKms = vRel.value_or(0.0);
        // v_inf is left empty when absent rather than copied from v_rel: they
        // are different quantities (v_inf excludes the Earth's gravitational
        // focusing, so v_inf < v_rel), and substituting one for the other would
        // silently corrupt a velocity query.
        a.vInfinityKms = detail::JsonTable::number(row, col.vInf);
        a.absoluteMagnitudeH = detail::JsonTable::number(row, col.h);
        a.diameterKm = detail::JsonTable::number(row, col.diameter);
        a.diameterSigmaKm = detail::JsonTable::number(row, col.diameterSigma);

        out.push_back(std::move(parsed));
        ++report.rowsAccepted;
    }
    return ParseStatus::success();
}

} // namespace neo
