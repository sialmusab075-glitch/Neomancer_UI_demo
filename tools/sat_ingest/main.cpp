// sat_ingest: download element sets from CelesTrak and write the satellite catalogue the app reads.
//
//   sat_ingest                                  stations + visual groups, plus the pinned ISS / Hubble / Tiangong
//   sat_ingest --offline                        rebuild the catalogue from the response cache, no network
//   sat_ingest --refresh                        download again (a copy younger than 2 hours is kept: CelesTrak's rule)
//   sat_ingest --from-tle FILE                  import a TLE / 3LE file instead; no network
//   sat_ingest --list                           print the stored catalogue and exit
//
// SATELLITES ARE VISUALIZATION ONLY: this tool, src/sat and docs/SATELLITES.md are separate from the NEO
// data layer. The catalogue is data/sat/elements.json, the raw responses are cached in data/sat/cache.

#include "sat/ingest/CelesTrakIngestor.h"
#include "sat/model/Epoch.h"
#include "sat/parse/TleParser.h"
#include "sat/store/Catalog.h"

#include "neo/ingest/Fetcher.h"
#include "neo/ingest/ResponseCache.h"
#include "neo/model/JulianDate.h"
#include "neo/net/WinHttpClient.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::printf(
        "sat_ingest - build the satellite catalogue from CelesTrak\n"
        "\n"
        "Usage: sat_ingest [options]\n"
        "  --data-dir DIR          where data lives (default: data); writes DIR/sat/elements.json\n"
        "  --groups A,B            CelesTrak groups to download (default: stations,visual)\n"
        "  --no-pinned             do not fetch the pinned ISS / Hubble / Tiangong by catalogue number\n"
        "  --offline               never use the network; rebuild from DIR/sat/cache\n"
        "  --refresh               download again, but keep any cached copy younger than --min-age\n"
        "  --min-age MINUTES       the refresh rule (default 120: CelesTrak regenerates sets about every 2 hours)\n"
        "  --max-attempts N        tries per request (default 5)\n"
        "  --min-interval MS       pause between requests (default 1000)\n"
        "  --from-tle FILE         import a TLE or 3LE file instead of downloading\n"
        "  --list                  print the stored catalogue and exit\n"
        "  --quiet                 less output\n"
        "  -h, --help\n");
}

bool nextValue(int argc, char** argv, int& i, const char* flag, std::string& out) {
    if (i + 1 >= argc) {
        std::printf("error: %s needs a value\n", flag);
        return false;
    }
    out = argv[++i];
    return true;
}

std::vector<std::string> splitCommas(const std::string& text) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            parts.push_back(item);
        }
    }
    return parts;
}

double nowJd() {
    double whole = 0.0;
    double fraction = 0.0;
    sat::utcJdNow(whole, fraction);
    return whole + fraction;
}

void printCatalog(const std::vector<sat::ElementSet>& sets, const sat::CatalogMeta& meta) {
    std::printf("catalogue: %zu satellites, created %s, source '%s'\n", sets.size(), meta.createdUtc.c_str(), meta.source.c_str());
    std::printf("%7s  %-26s %-19s %9s %8s %7s  %-6s %8s\n", "NORAD", "NAME", "EPOCH (UTC)", "PERIOD", "INCL", "ECC", "SGP4", "AGE");
    const double now = nowJd();
    for (const sat::ElementSet& s : sets) {
        const std::string epoch = sat::epochToIso(s.epochJdWhole, s.epochJdFraction).substr(0, 19);
        std::printf("%7u  %-26.26s %-19s %7.1f m %7.2f° %7.5f  %-6s %6.1f d\n", s.noradId, s.name.c_str(), epoch.c_str(), s.periodMinutes(),
                    s.inclinationDeg, s.eccentricity, s.deepSpace() ? "deep" : "near", now - s.epochJd());
    }
}

} // namespace

int main(int argc, char** argv) {
    sat::SatIngestOptions options;
    std::string dataDir = "data";
    std::string fromTle;
    neo::CacheMode mode = neo::CacheMode::Normal;
    neo::FetchPolicy policy;
    bool quiet = false;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string text;
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "--data-dir") {
            if (!nextValue(argc, argv, i, "--data-dir", dataDir)) return 2;
        } else if (arg == "--groups") {
            if (!nextValue(argc, argv, i, "--groups", text)) return 2;
            options.groups = splitCommas(text);
        } else if (arg == "--no-pinned") {
            options.pinned.clear();
        } else if (arg == "--offline") {
            mode = neo::CacheMode::Offline;
        } else if (arg == "--refresh") {
            mode = neo::CacheMode::Refresh;
        } else if (arg == "--min-age") {
            if (!nextValue(argc, argv, i, "--min-age", text)) return 2;
            options.minRefreshInterval = std::chrono::minutes(std::atoi(text.c_str()));
        } else if (arg == "--max-attempts") {
            if (!nextValue(argc, argv, i, "--max-attempts", text)) return 2;
            policy.maxAttempts = std::max(1, std::atoi(text.c_str()));
        } else if (arg == "--min-interval") {
            if (!nextValue(argc, argv, i, "--min-interval", text)) return 2;
            policy.minInterval = std::chrono::milliseconds(std::max(0, std::atoi(text.c_str())));
        } else if (arg == "--from-tle") {
            if (!nextValue(argc, argv, i, "--from-tle", fromTle)) return 2;
        } else if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::printf("error: unknown option '%s' (try --help)\n", arg.c_str());
            return 2;
        }
    }

    for (const std::string& group : options.groups) {
        if (!sat::validGroupName(group)) {
            std::printf("error: '%s' is not a valid CelesTrak group name (lower-case letters, digits and '-')\n", group.c_str());
            return 2;
        }
    }
    if (options.groups.empty() && fromTle.empty() && !listOnly) {
        std::printf("error: --groups needs at least one group\n");
        return 2;
    }

    const std::string satDir = dataDir + "/sat";
    const std::string cataloguePath = satDir + "/elements.json";
    std::error_code ec;
    std::filesystem::create_directories(satDir, ec);

    if (listOnly) {
        std::vector<sat::ElementSet> sets;
        sat::CatalogMeta meta;
        std::string error;
        if (!sat::loadCatalog(cataloguePath, sets, meta, error)) {
            std::printf("error: %s\n", error.c_str());
            return 1;
        }
        printCatalog(sets, meta);
        return 0;
    }

    std::vector<sat::ElementSet> sets;
    sat::CatalogMeta meta;
    meta.createdUtc = neo::utcNowIso();

    if (!fromTle.empty()) {
        std::ifstream file(fromTle, std::ios::binary);
        if (!file) {
            std::printf("error: cannot open %s\n", fromTle.c_str());
            return 1;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const sat::SetParseReport report = sat::parseTleText(buffer.str(), sets);
        std::printf("sat_ingest: imported %s: %zu accepted, %zu rejected\n", fromTle.c_str(), report.accepted, report.rejected);
        for (const auto& sample : report.rejectedSamples) {
            std::printf("  rejected #%zu: %s\n", sample.first, sample.second.c_str());
        }
        if (sets.empty()) {
            std::printf("error: no element sets in %s\n", fromTle.c_str());
            return 1;
        }
        std::sort(sets.begin(), sets.end(), [](const sat::ElementSet& a, const sat::ElementSet& b) { return a.noradId < b.noradId; });
        meta.source = "TLE file " + std::filesystem::path(fromTle).filename().string();
    } else {
        neo::WinHttpClient::Options httpOptions;
        httpOptions.userAgent = "SolSystemSim-Satellites/0.1 (semester project; C++/WinHTTP)";
        neo::WinHttpClient http(httpOptions);
        if (!http.valid() && mode != neo::CacheMode::Offline) {
            std::printf("error: could not open a WinHTTP session\n");
            return 1;
        }
        neo::ResponseCache cache(satDir + "/cache");
        neo::Fetcher fetcher(http, cache, mode, policy);
        const auto log = [quiet](const std::string& text) {
            if (!quiet) {
                std::printf("%s\n", text.c_str());
                std::fflush(stdout);
            }
        };
        fetcher.setLogFunction(log);

        std::printf("sat_ingest: %s mode, cache %s/cache\n",
                    mode == neo::CacheMode::Offline ? "offline" : mode == neo::CacheMode::Refresh ? "refresh" : "online", satDir.c_str());
        sat::CelesTrakIngestor ingestor(fetcher, cache, options);
        sat::SatIngestReport report;
        const bool ok = ingestor.run(sets, report);
        std::printf("%s", report.toText().c_str());
        {
            std::ofstream out(satDir + "/ingest_report.txt", std::ios::binary | std::ios::trunc);
            out << report.toText();
        }
        if (!ok) {
            std::printf("catalogue NOT written: %s\n", report.error.c_str());
            return 1;
        }
        meta.source = "CelesTrak GP (OMM JSON)";
        meta.groups = options.groups;
    }

    std::string error;
    if (!sat::saveCatalog(cataloguePath, sets, meta, error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s (%zu satellites)\n", cataloguePath.c_str(), sets.size());
    if (!quiet) {
        printCatalog(sets, meta);
    }
    return 0;
}
