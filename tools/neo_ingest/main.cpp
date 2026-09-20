// neo_ingest: downloads NEO objects (SBDB) and close approaches (CAD), parses,
// joins and validates them, and writes a validation report. Never run from the
// UI: it is a command-line tool, and the UI reads the database instead.
//
// Stage 3 stops at the in-memory dataset plus the report; writing neo.db is
// stage 4.

#include "neo/ingest/Fetcher.h"
#include "neo/ingest/Ingestor.h"
#include "neo/ingest/ResponseCache.h"
#include "neo/model/Dataset.h"
#include "neo/net/WinHttpClient.h"

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
