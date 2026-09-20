// neo_query: run one query against data/neo.db from the terminal.
//
//   neo_query --pha --from 2030-01-01 --to 2040-01-01 --max-dist-ld 5
//             --min-diam 0.14 --top 10 --sort dist --explain
//
// Prints a results table and, with --explain, the query plan. It reads the
// database only: no network, no SQL (the database is loaded into memory and the
// query runs on the project's own indexes).

#include "neo/model/JulianDate.h"
#include "neo/query/QueryEngine.h"
#include "neo/storage/Database.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::printf(
        "neo_query - query the local NEO database\n"
        "\n"
        "Usage: neo_query [options]\n"
        "\n"
        "Data\n"
        "  --db PATH               database file (default: data/neo.db)\n"
        "  --info                  print the index build report and exit\n"
        "\n"
        "Object filters\n"
        "  --designation X         exact primary designation (case-sensitive)\n"
        "  --name PREFIX           case-insensitive prefix of the name or designation\n"
        "  --pha | --not-pha       potentially hazardous flag (unknown flags match neither)\n"
        "  --neo | --not-neo       near-Earth flag\n"
        "  --kind asteroid|comet\n"
        "  --class APO,ATE         orbit class(es), comma separated, exact codes\n"
        "  --min-diam KM  --max-diam KM       diameter in km\n"
        "  --min-diam-m M --max-diam-m M      diameter in metres\n"
        "  --measured-only         only SBDB's measured diameters (default also uses H-estimates)\n"
        "  --include-unknown-diam  objects with no diameter at all also match\n"
        "  --min-h --max-h --min-moid --max-moid --min-a --max-a --min-e --max-e --min-i --max-i\n"
        "\n"
        "Approach filters (all applied to the SAME approach)\n"
        "  --from YYYY-MM-DD  --to YYYY-MM-DD   date window (--to includes the whole day)\n"
        "  --min-dist-au X --max-dist-au X      approach distance in au\n"
        "  --min-dist-ld X --max-dist-ld X      approach distance in lunar distances\n"
        "  --min-vel KMS --max-vel KMS          relative velocity in km/s\n"
        "  --grazing | --not-grazing            nominal distance below one Earth radius\n"
        "\n"
        "Output\n"
        "  --sort dist|date|vel|diam|h|moid|a|e|i|name|none   (default: none = index order)\n"
        "  --desc                  descending order\n"
        "  --top N                 keep the N best (0 = all)\n"
        "  --show N                rows to print (default 25)\n"
        "  --mode planned|fixed|naive|all   execution path (default planned; all compares them)\n"
        "  --explain               print the query plan\n"
        "  -h, --help\n");
}

bool need(int argc, int i, const char* flag) {
    if (i + 1 >= argc) {
        std::printf("error: %s needs a value\n", flag);
        return false;
    }
    return true;
}

bool toDouble(const char* text, const char* flag, double& out) {
    char* end = nullptr;
    out = std::strtod(text, &end);
    if (end == text || *end != '\0') {
        std::printf("error: %s expects a number, got '%s'\n", flag, text);
        return false;
    }
    return true;
}

bool toDate(const char* text, const char* flag, double& jd) {
    if (!neo::julianDateFromIsoDate(text, jd)) {
        std::printf("error: %s expects YYYY-MM-DD, got '%s'\n", flag, text);
        return false;
    }
    return true;
}

bool parseSort(const std::string& s, neo::SortField& out) {
    if (s == "dist")        out = neo::SortField::Distance;
    else if (s == "date")   out = neo::SortField::Date;
    else if (s == "vel")    out = neo::SortField::Velocity;
    else if (s == "diam")   out = neo::SortField::Diameter;
    else if (s == "h")      out = neo::SortField::AbsoluteMagnitude;
    else if (s == "moid")   out = neo::SortField::Moid;
    else if (s == "a")      out = neo::SortField::SemiMajorAxis;
    else if (s == "e")      out = neo::SortField::Eccentricity;
    else if (s == "i")      out = neo::SortField::Inclination;
    else if (s == "name")   out = neo::SortField::Designation;
    else if (s == "none")   out = neo::SortField::None;
    else return false;
    return true;
}

std::vector<std::string> splitCommas(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

void printTable(const neo::QueryEngine& engine, const neo::QueryResult& r, std::size_t show) {
    const neo::Dataset& ds = engine.dataset();
    std::printf("\n%-3s %-13s %-14s %-5s %-4s %6s  %-12s %5s  %-10s %-11s %-7s %-7s\n", "#", "DESIGNATION", "NAME",
                "CLASS", "PHA", "H", "DIAM (km)", "APPR", "DATE", "DIST (au)", "(LD)", "V_REL");
    std::size_t printed = 0;
    for (const neo::ResultRow& row : r.rows) {
        if (printed >= show) {
            break;
        }
        const neo::Asteroid& a = ds.records()[row.object].object;

        char diam[24];
        if (a.physical.diameterKm) {
            std::snprintf(diam, sizeof diam, "%.3f", *a.physical.diameterKm);
        } else if (const std::optional<double> est = a.physical.estimatedDiameterKm()) {
            std::snprintf(diam, sizeof diam, "~%.3f (est)", *est); // '~' = derived from H, not measured
        } else {
            std::snprintf(diam, sizeof diam, "?");
        }
        char h[16];
        if (a.physical.absoluteMagnitudeH) {
            std::snprintf(h, sizeof h, "%.2f", *a.physical.absoluteMagnitudeH);
        } else {
            std::snprintf(h, sizeof h, "?");
        }
        const char* pha = !a.classification.isPHA ? "?" : (*a.classification.isPHA ? "yes" : "no");

        // The matching approach that passed closest to Earth.
        const neo::CloseApproach* best = nullptr;
        for (const std::uint32_t* it = r.approachesBegin(row); it != r.approachesEnd(row); ++it) {
            const neo::CloseApproach& c = ds.approaches()[*it];
            if (best == nullptr || c.distanceAU < best->distanceAU) {
                best = &c;
            }
        }
        char when[16] = "-";
        char dist[16] = "-";
        char ld[16] = "-";
        char vel[16] = "-";
        if (best != nullptr) {
            std::snprintf(when, sizeof when, "%s", neo::formatJulianDay(best->jdTdb).c_str());
            std::snprintf(dist, sizeof dist, "%.6f", best->distanceAU);
            std::snprintf(ld, sizeof ld, "%.2f", best->distanceAU / neo::kLunarDistanceAU);
            std::snprintf(vel, sizeof vel, "%.2f", best->relVelocityKms);
        }
        std::printf("%-3zu %-13.13s %-14.14s %-5.5s %-4s %6s  %-12s %5u  %-10s %-11s %-7s %-7s\n", printed + 1,
                    a.pdes.c_str(), a.name.empty() ? "" : a.name.c_str(), a.classification.orbitClass.c_str(), pha, h,
                    diam, row.approachCount, when, dist, ld, vel);
        ++printed;
    }
    if (r.rows.empty()) {
        std::printf("(no matches)\n");
    }
    std::printf("\n%zu object(s) matched with %zu matching approach(es); showing %zu.\n", r.totalObjects,
                r.totalApproaches, printed);
    std::printf("Rows show the matching approach with the smallest distance. '~' marks a diameter estimated from H.\n");
}

bool sameRows(const neo::QueryResult& a, const neo::QueryResult& b) {
    if (a.rows.size() != b.rows.size() || a.totalObjects != b.totalObjects || a.totalApproaches != b.totalApproaches) {
        return false;
    }
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        if (a.rows[i].object != b.rows[i].object || a.rows[i].approachCount != b.rows[i].approachCount) {
            return false;
        }
        for (std::uint32_t k = 0; k < a.rows[i].approachCount; ++k) {
            if (a.approachPool[a.rows[i].approachBegin + k] != b.approachPool[b.rows[i].approachBegin + k]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string dbPath = "data/neo.db";
    std::string mode = "planned";
    bool explain = false;
    bool info = false;
    std::size_t show = 25;
    neo::Query q;
    q.diameterMode = neo::DiameterMode::MeasuredOrEstimated;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        double v = 0.0;
        double jd = 0.0;
        auto lo = [](neo::Range& r, double x) { r.lo = x; };
        auto hi = [](neo::Range& r, double x) { r.hi = x; };
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "--db") {
            if (!need(argc, i, "--db")) return 2;
            dbPath = argv[++i];
        } else if (arg == "--info") {
            info = true;
        } else if (arg == "--explain") {
            explain = true;
        } else if (arg == "--desc") {
            q.direction = neo::SortDirection::Descending;
        } else if (arg == "--pha") {
            q.pha = neo::TriState::Yes;
        } else if (arg == "--not-pha") {
            q.pha = neo::TriState::No;
        } else if (arg == "--neo") {
            q.neo = neo::TriState::Yes;
        } else if (arg == "--not-neo") {
            q.neo = neo::TriState::No;
        } else if (arg == "--grazing") {
            q.grazing = neo::TriState::Yes;
        } else if (arg == "--not-grazing") {
            q.grazing = neo::TriState::No;
        } else if (arg == "--measured-only") {
            q.diameterMode = neo::DiameterMode::MeasuredOnly;
        } else if (arg == "--include-unknown-diam") {
            q.diameterMode = neo::DiameterMode::IncludeUnknown;
        } else if (arg == "--designation") {
            if (!need(argc, i, "--designation")) return 2;
            q.designation = argv[++i];
        } else if (arg == "--name") {
            if (!need(argc, i, "--name")) return 2;
            q.namePrefix = argv[++i];
        } else if (arg == "--kind") {
            if (!need(argc, i, "--kind")) return 2;
            const std::string k = argv[++i];
            if (k == "asteroid") q.kind = neo::ObjectKind::Asteroid;
            else if (k == "comet") q.kind = neo::ObjectKind::Comet;
            else {
                std::printf("error: --kind expects asteroid or comet\n");
                return 2;
            }
        } else if (arg == "--class") {
            if (!need(argc, i, "--class")) return 2;
            q.orbitClasses = splitCommas(argv[++i]);
        } else if (arg == "--sort") {
            if (!need(argc, i, "--sort")) return 2;
            if (!parseSort(argv[++i], q.sortBy)) {
                std::printf("error: unknown sort field '%s'\n", argv[i]);
                return 2;
            }
        } else if (arg == "--top") {
            if (!need(argc, i, "--top")) return 2;
            q.topK = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--show") {
            if (!need(argc, i, "--show")) return 2;
            show = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--mode") {
            if (!need(argc, i, "--mode")) return 2;
            mode = argv[++i];
        } else if (arg == "--from") {
            if (!need(argc, i, "--from") || !toDate(argv[++i], "--from", jd)) return 2;
            lo(q.dateJd, jd);
        } else if (arg == "--to") {
            if (!need(argc, i, "--to") || !toDate(argv[++i], "--to", jd)) return 2;
            hi(q.dateJd, jd + 0.99999); // the whole day
        } else if (arg == "--min-dist-au" || arg == "--max-dist-au" || arg == "--min-dist-ld" ||
                   arg == "--max-dist-ld" || arg == "--min-vel" || arg == "--max-vel" || arg == "--min-diam" ||
                   arg == "--max-diam" || arg == "--min-diam-m" || arg == "--max-diam-m" || arg == "--min-h" ||
                   arg == "--max-h" || arg == "--min-moid" || arg == "--max-moid" || arg == "--min-a" ||
                   arg == "--max-a" || arg == "--min-e" || arg == "--max-e" || arg == "--min-i" || arg == "--max-i") {
            if (!need(argc, i, arg.c_str()) || !toDouble(argv[++i], arg.c_str(), v)) return 2;
            const bool isMin = arg.rfind("--min", 0) == 0;
            auto set = [&](neo::Range& r, double x) { isMin ? lo(r, x) : hi(r, x); };
            if (arg.find("dist-au") != std::string::npos) set(q.distanceAU, v);
            else if (arg.find("dist-ld") != std::string::npos) set(q.distanceAU, v * neo::kLunarDistanceAU);
            else if (arg.find("vel") != std::string::npos) set(q.velocityKms, v);
            else if (arg.find("diam-m") != std::string::npos) set(q.diameterKm, v / 1000.0);
            else if (arg.find("diam") != std::string::npos) set(q.diameterKm, v);
            else if (arg.find("moid") != std::string::npos) set(q.moidAU, v);
            else if (arg == "--min-h" || arg == "--max-h") set(q.absoluteMagnitude, v);
            else if (arg == "--min-a" || arg == "--max-a") set(q.semiMajorAxisAU, v);
            else if (arg == "--min-e" || arg == "--max-e") set(q.eccentricity, v);
            else set(q.inclinationDeg, v);
        } else {
            std::printf("error: unknown option '%s' (try --help)\n", arg.c_str());
            return 2;
        }
    }

    neo::Dataset dataset;
    neo::DatabaseMeta meta;
    const neo::DbStatus loaded = neo::loadDatabase(dbPath, dataset, meta);
    if (!loaded) {
        std::printf("error: %s\n", loaded.error.c_str());
        return 1;
    }
    std::printf("database %s: %zu objects, %zu approaches (created %s)\n", dbPath.c_str(), dataset.objectCount(),
                dataset.approachCount(), meta.createdUtc.c_str());

    neo::QueryEngine engine(dataset);
    engine.build();
    std::printf("indexes built in %.0f ms (%.1f MB)\n", engine.totalBuildMs(),
                static_cast<double>(engine.totalIndexBytes()) / (1024.0 * 1024.0));

    if (info) {
        std::printf("\n%-46s %10s %10s %10s\n", "index", "entries", "MB", "build ms");
        for (const neo::IndexBuildInfo& b : engine.buildReport()) {
            std::printf("%-46s %10zu %10.3f %10.2f\n", b.name.c_str(), b.entries,
                        static_cast<double>(b.bytes) / (1024.0 * 1024.0), b.buildMs);
        }
        return 0;
    }

    const std::vector<std::string> problems = neo::validate(q);
    if (!problems.empty()) {
        for (const std::string& p : problems) {
            std::printf("invalid query: %s\n", p.c_str());
        }
        return 2;
    }

    if (mode == "all") {
        const neo::QueryResult naive = engine.run(q, neo::ExecMode::Naive);
        const neo::QueryResult fixed = engine.run(q, neo::ExecMode::FixedOrder);
        const neo::QueryResult planned = engine.run(q, neo::ExecMode::Planned);
        printTable(engine, planned, show);
        std::printf("\nExecution paths compared:\n");
        std::printf("  %-12s %9.3f ms   %s\n", "naive", naive.stats.totalMs, "full linear scan (the oracle)");
        std::printf("  %-12s %9.3f ms   driver: %s\n", "fixed-order", fixed.stats.totalMs, fixed.stats.driverText.c_str());
        std::printf("  %-12s %9.3f ms   driver: %s\n", "planned", planned.stats.totalMs, planned.stats.driverText.c_str());
        std::printf("  all three agree: %s\n", sameRows(naive, fixed) && sameRows(naive, planned) ? "yes" : "NO");
        if (explain) {
            std::printf("\n%s", planned.stats.explain().c_str());
        }
        return 0;
    }

    neo::ExecMode m = neo::ExecMode::Planned;
    if (mode == "naive") m = neo::ExecMode::Naive;
    else if (mode == "fixed") m = neo::ExecMode::FixedOrder;
    else if (mode != "planned") {
        std::printf("error: --mode expects planned, fixed, naive or all\n");
        return 2;
    }
    const neo::QueryResult result = engine.run(q, m);
    if (!result.ok) {
        for (const std::string& e : result.errors) {
            std::printf("error: %s\n", e.c_str());
        }
        return 2;
    }
    printTable(engine, result, show);
    if (explain) {
        std::printf("\n%s", result.stats.explain().c_str());
    }
    return 0;
}
