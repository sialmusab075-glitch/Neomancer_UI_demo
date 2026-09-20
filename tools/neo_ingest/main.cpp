// neo_ingest: downloads NEO objects (SBDB) and close approaches (CAD), parses,
// joins and validates them, and writes a validation report. Never run from the
// UI: it is a command-line tool, and the UI reads the database instead.
//
// Stage 3 stops at the in-memory dataset plus the report; writing neo.db is
// stage 4.

#include "neo/ingest/Fetcher.h"
#include "neo/ingest/Ingestor.h"
#include "neo/ingest/CadParser.h"
#include "neo/ingest/ResponseCache.h"
#include "neo/ingest/SbdbParser.h"
#include "neo/model/Dataset.h"
#include "neo/model/Hash.h"
#include "neo/net/WinHttpClient.h"
#include "neo/storage/Database.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::printf(
        "neo_ingest - download and validate NEO data from NASA/JPL\n"
        "\n"
        "Usage: neo_ingest [options]\n"
        "\n"
        "Data and cache\n"
        "  --data-dir DIR        where reports and the cache live (default: data)\n"
        "  --cache-dir DIR       raw response cache (default: <data-dir>/cache)\n"
        "  --offline             never use the network; run entirely from the cache\n"
        "  --refresh             ignore cached responses and download again\n"
        "  --rebuild-db          rebuild neo.db from the cache, no network (implies --offline)\n"
        "  --no-db               do not write neo.db\n"
        "  --check-db N          after writing, load neo.db N times and report the median\n"
        "  --no-resume           ignore the progress file and start from the beginning\n"
        "\n"
        "What to fetch\n"
        "  --skip-sbdb           do not fetch objects\n"
        "  --skip-cad            do not fetch close approaches\n"
        "  --date-min YYYY-MM-DD CAD window start (default: 1950-01-01)\n"
        "  --date-max YYYY-MM-DD CAD window end   (default: 2150-01-01)\n"
        "  --dist-max AU         CAD maximum approach distance (default: 0.05)\n"
        "  --window-years N      initial CAD window width in years (default: 5)\n"
        "  --page-size N         rows per CAD request (default: 5000)\n"
        "  --sbdb-page-size N    objects per SBDB request (default: 5000)\n"
        "  --threshold N         ask before downloading more than N CAD rows (default: 500000)\n"
        "  --yes                 accept the row count without asking\n"
        "\n"
        "Politeness\n"
        "  --min-interval MS     minimum gap between requests (default: 1000)\n"
        "  --max-attempts N      attempts per request, 5xx/timeouts only (default: 5)\n"
        "  --timeout MS          receive timeout (default: 60000)\n"
        "  --quiet               only print the summary\n"
        "  -h, --help            this text\n");
}

// Returns false (and explains) when the value is missing or not a number.
bool nextValue(int argc, char** argv, int& i, const char* flag, std::string& out) {
    if (i + 1 >= argc) {
        std::printf("error: %s needs a value\n", flag);
        return false;
    }
    out = argv[++i];
    return true;
}

bool nextSize(int argc, char** argv, int& i, const char* flag, std::size_t& out) {
    std::string text;
    if (!nextValue(argc, argv, i, flag, text)) {
        return false;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || value < 0) {
        std::printf("error: %s expects a non-negative integer, got '%s'\n", flag, text.c_str());
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    neo::IngestOptions options;
    std::string dataDir = "data";
    std::string cacheDir;
    neo::CacheMode mode = neo::CacheMode::Normal;
    neo::FetchPolicy policy;
    neo::WinHttpClient::Options httpOptions;
    bool quiet = false;
    bool writeDb = true;
    std::size_t checkDbLoads = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::size_t size = 0;
        std::string text;
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "--data-dir") {
            if (!nextValue(argc, argv, i, "--data-dir", dataDir)) return 2;
        } else if (arg == "--cache-dir") {
            if (!nextValue(argc, argv, i, "--cache-dir", cacheDir)) return 2;
        } else if (arg == "--offline") {
            mode = neo::CacheMode::Offline;
        } else if (arg == "--rebuild-db") {
            // Everything needed is already cached, so this needs no network.
            mode = neo::CacheMode::Offline;
            writeDb = true;
        } else if (arg == "--no-db") {
            writeDb = false;
        } else if (arg == "--check-db") {
            if (!nextSize(argc, argv, i, "--check-db", checkDbLoads)) return 2;
        } else if (arg == "--refresh") {
            mode = neo::CacheMode::Refresh;
        } else if (arg == "--no-resume") {
            options.resume = false;
        } else if (arg == "--skip-sbdb") {
            options.skipSbdb = true;
        } else if (arg == "--skip-cad") {
            options.skipCad = true;
        } else if (arg == "--yes") {
            options.assumeYes = true;
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--date-min") {
            if (!nextValue(argc, argv, i, "--date-min", options.cadDateMin)) return 2;
        } else if (arg == "--date-max") {
            if (!nextValue(argc, argv, i, "--date-max", options.cadDateMax)) return 2;
        } else if (arg == "--dist-max") {
            if (!nextValue(argc, argv, i, "--dist-max", text)) return 2;
            options.cadDistMaxAU = std::atof(text.c_str());
            if (options.cadDistMaxAU <= 0.0) {
                std::printf("error: --dist-max must be positive\n");
                return 2;
            }
        } else if (arg == "--window-years") {
            if (!nextSize(argc, argv, i, "--window-years", size)) return 2;
            options.cadWindowYears = static_cast<int>(size);
        } else if (arg == "--page-size") {
            if (!nextSize(argc, argv, i, "--page-size", options.cadPageSize)) return 2;
        } else if (arg == "--sbdb-page-size") {
            if (!nextSize(argc, argv, i, "--sbdb-page-size", options.sbdbPageSize)) return 2;
        } else if (arg == "--threshold") {
            if (!nextSize(argc, argv, i, "--threshold", options.confirmThreshold)) return 2;
        } else if (arg == "--min-interval") {
            if (!nextSize(argc, argv, i, "--min-interval", size)) return 2;
            policy.minInterval = std::chrono::milliseconds(static_cast<long long>(size));
        } else if (arg == "--max-attempts") {
            if (!nextSize(argc, argv, i, "--max-attempts", size)) return 2;
            policy.maxAttempts = static_cast<int>(size > 0 ? size : 1);
        } else if (arg == "--timeout") {
            if (!nextSize(argc, argv, i, "--timeout", size)) return 2;
            httpOptions.receiveTimeout = std::chrono::milliseconds(static_cast<long long>(size));
        } else {
            std::printf("error: unknown option '%s' (try --help)\n", arg.c_str());
            return 2;
        }
    }

    if (cacheDir.empty()) {
        cacheDir = dataDir + "/cache";
    }
    options.progressPath = dataDir + "/ingest_progress.json";

    neo::WinHttpClient http(httpOptions);
    if (!http.valid() && mode != neo::CacheMode::Offline) {
        std::printf("error: could not open a WinHTTP session\n");
        return 1;
    }
    neo::ResponseCache cache(cacheDir);
    neo::Fetcher fetcher(http, cache, mode, policy);

    const auto log = [quiet](const std::string& text) {
        if (!quiet) {
            std::printf("%s\n", text.c_str());
            std::fflush(stdout);
        }
    };
    fetcher.setLogFunction(log);

    neo::Ingestor ingestor(fetcher, options);
    ingestor.setLogFunction(log);

    std::printf("neo_ingest: %s mode, cache %s\n",
                mode == neo::CacheMode::Offline ? "offline" : mode == neo::CacheMode::Refresh ? "refresh" : "online",
                cacheDir.c_str());

    neo::Dataset dataset;
    neo::IngestReport report;
    const bool ok = ingestor.run(dataset, report);

    // Persist the dataset. Only a complete, successful run may replace neo.db:
    // a half-downloaded dataset would look authoritative while missing rows.
    const std::string dbPath = dataDir + "/neo.db";
    if (ok && writeDb) {
        neo::DatabaseMeta meta;
        meta.sbdbApiVersion =
            report.sbdb.signatureVersion.empty() ? neo::kSbdbApiVersion : report.sbdb.signatureVersion;
        meta.cadApiVersion = report.cad.signatureVersion.empty() ? neo::kCadApiVersion : report.cad.signatureVersion;
        meta.cadDateMin = report.cadDateMin;
        meta.cadDateMax = report.cadDateMax;
        meta.cadDistMaxAU = report.cadDistMaxAU;
        meta.reportChecksum = neo::fnv1a64Hex(report.toJson()); // ties the file to this run

        const auto dbStarted = std::chrono::steady_clock::now();
        const neo::DbStatus status = neo::saveDatabase(dataset, meta, dbPath);
        report.dbWriteSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - dbStarted).count();
        if (!status) {
            std::printf("error writing %s: %s\n", dbPath.c_str(), status.error.c_str());
            return 1;
        }
        report.dbPath = dbPath;
        report.dbBytes = neo::databaseFileSize(dbPath);
        std::printf("\nneo.db: %zu objects, %zu approaches, %.2f MB in %.2f s\n", dataset.objectCount(),
                    dataset.approachCount(), static_cast<double>(report.dbBytes) / (1024.0 * 1024.0),
                    report.dbWriteSeconds);
    }

    // Load it back: both a check that the file is readable and the load-time
    // number quoted in the plan. The median of N runs, not the best or the mean.
    if (ok && checkDbLoads > 0) {
        std::vector<double> times;
        times.reserve(checkDbLoads);
        for (std::size_t run = 0; run < checkDbLoads; ++run) {
            neo::Dataset loaded;
            neo::DatabaseMeta meta;
            const auto loadStarted = std::chrono::steady_clock::now();
            const neo::DbStatus status = neo::loadDatabase(dbPath, loaded, meta);
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStarted).count();
            if (!status) {
                std::printf("error loading %s: %s\n", dbPath.c_str(), status.error.c_str());
                return 1;
            }
            times.push_back(seconds);
            std::printf("load %zu/%zu: %zu objects, %zu approaches in %.3f s\n", run + 1, checkDbLoads,
                        loaded.objectCount(), loaded.approachCount(), seconds);
        }
        std::sort(times.begin(), times.end());
        std::printf("median load time: %.3f s over %zu loads (file %.2f MB)\n", times[times.size() / 2],
                    times.size(), static_cast<double>(neo::databaseFileSize(dbPath)) / (1024.0 * 1024.0));
    }

    std::string error;
    if (!neo::writeIngestReports(report, dataDir, error)) {
        std::printf("warning: %s\n", error.c_str());
    } else {
        std::printf("\nreports: %s/ingest_report.txt and .json\n", dataDir.c_str());
    }
    std::printf("\n%s\n", report.toText().c_str());

    if (!ok) {
        std::printf("FAILED: %s\n", report.error.c_str());
        return 1;
    }
    return 0;
}
