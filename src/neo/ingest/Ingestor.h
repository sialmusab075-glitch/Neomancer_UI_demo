#pragma once

#include "neo/ingest/Fetcher.h"
#include "neo/ingest/ParseReport.h"
#include "neo/model/Dataset.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace neo {

// Endpoints, in one place.
constexpr const char* kSbdbEndpoint = "https://ssd-api.jpl.nasa.gov/sbdb_query.api";
constexpr const char* kCadEndpoint = "https://ssd-api.jpl.nasa.gov/cad.api";

struct IngestOptions {
    // --- SBDB ---------------------------------------------------------------
    std::size_t sbdbPageSize = 5000; // limit / limit-from paging
    bool        skipSbdb = false;

    // --- CAD ----------------------------------------------------------------
    // Defaults for v1. The wider 1900-01-01 .. 2200-01-01 at 0.2 AU stays
    // available through the CLI flags.
    std::string cadDateMin = "1950-01-01";
    std::string cadDateMax = "2150-01-01";
    double      cadDistMaxAU = 0.05;
    int         cadWindowYears = 5;      // initial window width
    std::size_t cadPageSize = 5000;      // rows per request; a window carrying
                                         // more is split in half instead
    bool        skipCad = false;

    // Above this many CAD rows the run stops unless assumeYes is set, so a
    // careless --dist-max cannot start a multi-million-row download.
    std::size_t confirmThreshold = 500000;
    bool        assumeYes = false;

    // Progress file for resuming an interrupted run. Empty disables it.
    std::string progressPath;
    bool        resume = true;
};

// Everything the run produced, beyond the dataset itself.
struct IngestReport {
    std::string startedUtc;
    std::string finishedUtc;
    double      elapsedSeconds = 0.0;
    std::string mode; // online | offline | refresh

    // Echo of the request parameters, so a report explains its own numbers.
    std::string cadDateMin, cadDateMax;
    double      cadDistMaxAU = 0.0;
    int         cadWindowYears = 0;
    std::size_t sbdbPageSize = 0, cadPageSize = 0;

    std::size_t      sbdbTotalReported = 0; // count-first result
    std::size_t      sbdbPages = 0;
    ValidationReport sbdb;

    std::size_t      cadTotalReported = 0;
    std::size_t      cadWindows = 0;       // windows actually fetched
    std::size_t      cadWindowSplits = 0;  // windows that had to be halved
    std::size_t      duplicateRowsDropped = 0; // same object + time, from a window edge
    ValidationReport cad;

    JoinReport join;

    std::size_t objects = 0;
    std::size_t approaches = 0;
    std::size_t objectsWithoutApproaches = 0;
    std::size_t grazingOrImpact = 0; // approaches closer than one Earth radius
    std::size_t unpropagatable = 0;     // e >= 1
    std::size_t measuredDiameters = 0;
    std::size_t estimatedDiameters = 0; // no measurement, but H is known
    std::size_t noDiameterAtAll = 0;

    FetchStats  fetch;

    // Filled in by neo_ingest after it writes neo.db (stage 4).
    std::string dbPath;
    std::size_t dbBytes = 0;
    double      dbWriteSeconds = 0.0;

    bool        ok = false;
    std::string error;

    std::string toText() const;
    std::string toJson() const;
};

// Writes <dir>/ingest_report.json and <dir>/ingest_report.txt.
bool writeIngestReports(const IngestReport& report, const std::string& dir, std::string& error);

// Downloads (or replays from cache) SBDB objects and CAD approaches, parses and
// joins them. Never touches the network itself: everything goes through Fetcher.
class Ingestor {
public:
    using LogFn = std::function<void(const std::string&)>;

    Ingestor(Fetcher& fetcher, IngestOptions options);

    void setLogFunction(LogFn fn) { log_ = std::move(fn); }

    // Fills `out` and `report`. Returns false on a fatal error (a page that
    // could not be fetched, an unsupported API version, or a refused
    // confirmation); the report still describes how far the run got.
    bool run(Dataset& out, IngestReport& report);

    // URL builders, exposed so tests can serve exactly these URLs.
    std::string sbdbCountUrl() const;
    std::string sbdbPageUrl(std::size_t limitFrom) const;
    std::string cadCountUrl() const;
    std::string cadWindowUrl(const std::string& dateMin, const std::string& dateMax) const;

private:
    struct Window {
        std::string minIso, maxIso;
        double      minJd = 0.0, maxJd = 0.0;
    };

    bool fetchObjects(std::vector<Asteroid>& objects, IngestReport& report);
    bool fetchApproaches(std::vector<ParsedApproach>& rows, IngestReport& report);
    std::vector<Window> makeWindows(std::string& error) const;
    void logLine(const std::string& text) const;

    Fetcher&      fetcher_;
    IngestOptions options_;
    LogFn         log_;

    // Resume marks, loaded from and written to the progress file.
    std::size_t resumeSbdbFrom_ = 0;
    double      resumeCadStartJd_ = 0.0;
    bool        progressLoaded_ = false;

    bool loadProgress();
    void saveProgress() const;
    std::string runSignature() const;
};

} // namespace neo
