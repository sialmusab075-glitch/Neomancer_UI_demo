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
        // The 3-sigma bounds and v_inf are always present in practice; when a
        // response omits them the nominal value stands in, so a range query can
        // never see a spurious 0 AU or 0 km/s.
        a.distanceMinAU = detail::JsonTable::number(row, col.distMin).value_or(a.distanceAU);
        a.distanceMaxAU = detail::JsonTable::number(row, col.distMax).value_or(a.distanceAU);
        a.relVelocityKms = vRel.value_or(0.0);
        a.vInfinityKms = detail::JsonTable::number(row, col.vInf).value_or(a.relVelocityKms);
        a.absoluteMagnitudeH = detail::JsonTable::number(row, col.h);
        a.diameterKm = detail::JsonTable::number(row, col.diameter);
        a.diameterSigmaKm = detail::JsonTable::number(row, col.diameterSigma);

        out.push_back(std::move(parsed));
        ++report.rowsAccepted;
    }
    return ParseStatus::success();
}

} // namespace neo
