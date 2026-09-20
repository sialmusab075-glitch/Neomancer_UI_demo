#include "neo/ingest/Ingestor.h"

#include "neo/ingest/CadParser.h"
#include "neo/ingest/SbdbParser.h"
#include "neo/model/JulianDate.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <utility>

namespace neo {

namespace {

std::string num(double v, int decimals = 4) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    // Trim trailing zeros so 0.05 stays "0.05" in the URL.
    while (s.size() > 1 && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

std::string Ingestor::sbdbCountUrl() const {
    // No fields requested: SBDB answers with the count of matching objects only.
    return std::string(kSbdbEndpoint) + "?sb-group=neo";
}

std::string Ingestor::sbdbPageUrl(std::size_t limitFrom) const {
    // sort=spkid makes paging deterministic. JPL warns the underlying database
    // can change between calls, so a stable sort key is the best we can do:
    // without it, an insert between pages could shift rows across page edges.
    return std::string(kSbdbEndpoint) + "?fields=" + kSbdbFields + "&full-prec=1&sb-group=neo&sort=spkid" +
           "&limit=" + std::to_string(options_.sbdbPageSize) + "&limit-from=" + std::to_string(limitFrom);
}

std::string Ingestor::cadCountUrl() const {
    return std::string(kCadEndpoint) + "?date-min=" + options_.cadDateMin + "&date-max=" + options_.cadDateMax +
           "&dist-max=" + num(options_.cadDistMaxAU, 6) + "&neo=true&total-only=true";
}

std::string Ingestor::cadWindowUrl(const std::string& dateMin, const std::string& dateMax) const {
    // neo=true keeps the download to objects SBDB's neo group also returns, so
    // the join has something to match; diameter/fullname are the documented
    // optional columns. limit makes the response report "total", which is how a
    // window learns it is too wide (see fetchApproaches).
    return std::string(kCadEndpoint) + "?date-min=" + dateMin + "&date-max=" + dateMax +
           "&dist-max=" + num(options_.cadDistMaxAU, 6) + "&neo=true&diameter=true&fullname=true&sort=date" +
           "&limit=" + std::to_string(options_.cadPageSize);
}

// ---------------------------------------------------------------------------
// Ingestor
// ---------------------------------------------------------------------------

Ingestor::Ingestor(Fetcher& fetcher, IngestOptions options) : fetcher_(fetcher), options_(std::move(options)) {}

void Ingestor::logLine(const std::string& text) const {
    if (log_) {
        log_(text);
    }
}

std::string Ingestor::runSignature() const {
    // The progress file only applies to an identical request set.
    char buf[256];
    std::snprintf(buf, sizeof buf, "sbdb:%zu|cad:%s..%s|dist:%s|win:%d|page:%zu", options_.sbdbPageSize,
                  options_.cadDateMin.c_str(), options_.cadDateMax.c_str(), num(options_.cadDistMaxAU, 6).c_str(),
                  options_.cadWindowYears, options_.cadPageSize);
    return buf;
}

bool Ingestor::loadProgress() {
    resumeSbdbFrom_ = 0;
    resumeCadStartJd_ = 0.0;
    progressLoaded_ = false;
    if (options_.progressPath.empty() || !options_.resume) {
        return false;
    }
    std::ifstream in(options_.progressPath, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("signature", std::string()) != runSignature()) {
        logLine("progress: file does not match this run's parameters, starting fresh");
        return false;
    }
    resumeSbdbFrom_ = j.value("sbdb_next_from", std::size_t{0});
    resumeCadStartJd_ = j.value("cad_next_start_jd", 0.0);
    progressLoaded_ = true;
    if (resumeSbdbFrom_ > 0 || resumeCadStartJd_ > 0.0) {
        logLine("progress: resuming (SBDB from record " + std::to_string(resumeSbdbFrom_) +
                (resumeCadStartJd_ > 0.0 ? ", CAD from " + formatJulianDay(resumeCadStartJd_) : std::string()) + ")");
    }
    return true;
}

void Ingestor::saveProgress() const {
    if (options_.progressPath.empty()) {
        return;
    }
    const std::filesystem::path path(options_.progressPath);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    nlohmann::json j;
    j["signature"] = runSignature();
    j["sbdb_next_from"] = resumeSbdbFrom_;
    j["cad_next_start_jd"] = resumeCadStartJd_;
    j["updated_utc"] = utcNowIso();
    std::ofstream out(options_.progressPath, std::ios::binary | std::ios::trunc);
    if (out) {
        const std::string text = j.dump(2);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

// --- SBDB ------------------------------------------------------------------

bool Ingestor::fetchObjects(std::vector<Asteroid>& objects, IngestReport& report) {
    // Count first, so the log says up front how much work this is.
    const Fetcher::Result count = fetcher_.get(sbdbCountUrl(), resumeSbdbFrom_ > 0);
    if (!count.ok) {
        report.error = "SBDB count request failed: " + count.error;
        return false;
    }
    {
        ValidationReport probe;
        std::vector<Asteroid> none;
        const ParseStatus status = parseSbdbObjects(count.body, none, probe);
        if (!status) {
            report.error = status.error;
            return false;
        }
        report.sbdbTotalReported = probe.declaredCount;
    }
    logLine("SBDB: " + std::to_string(report.sbdbTotalReported) + " NEO objects to fetch, " +
            std::to_string(options_.sbdbPageSize) + " per page");

    std::size_t from = 0;
    while (report.sbdbTotalReported == 0 || from < report.sbdbTotalReported) {
        const std::string url = sbdbPageUrl(from);
        // A page this run already completed is served from the cache even in
        // --refresh mode: resuming must not re-download what it just fetched.
        const bool alreadyDone = progressLoaded_ && from < resumeSbdbFrom_;
        const Fetcher::Result page = fetcher_.get(url, alreadyDone);
        if (!page.ok) {
            report.error = "SBDB page at record " + std::to_string(from) + " failed: " + page.error;
            return false;
        }
        ValidationReport pageReport;
        const std::size_t before = objects.size();
        const ParseStatus status = parseSbdbObjects(page.body, objects, pageReport);
        if (!status) {
            report.error = status.error;
            return false;
        }
        report.sbdb.merge(pageReport);
        ++report.sbdbPages;
        const std::size_t added = objects.size() - before;
        logLine("SBDB: page from " + std::to_string(from) + " -> " + std::to_string(pageReport.rowsSeen) +
                " rows, " + std::to_string(added) + " objects (total " + std::to_string(objects.size()) + ")");

        from += options_.sbdbPageSize;
        if (from > resumeSbdbFrom_) {
            resumeSbdbFrom_ = from;
            saveProgress();
        }
        if (pageReport.rowsSeen < options_.sbdbPageSize) {
            break; // last page
        }
    }
    return true;
}

// --- CAD -------------------------------------------------------------------

std::vector<Ingestor::Window> Ingestor::makeWindows(std::string& error) const {
    std::vector<Window> windows;
    double minJd = 0.0, maxJd = 0.0;
    if (!julianDateFromIsoDate(options_.cadDateMin, minJd)) {
        error = "invalid --date-min '" + options_.cadDateMin + "' (expected YYYY-MM-DD)";
        return windows;
    }
    if (!julianDateFromIsoDate(options_.cadDateMax, maxJd)) {
        error = "invalid --date-max '" + options_.cadDateMax + "' (expected YYYY-MM-DD)";
        return windows;
    }
    if (maxJd <= minJd) {
        error = "--date-max must be later than --date-min";
        return windows;
    }
    const int years = std::max(1, options_.cadWindowYears);
    const int startYear = std::stoi(options_.cadDateMin.substr(0, 4));

    std::string currentIso = options_.cadDateMin;
    double currentJd = minJd;
    int boundaryYear = startYear + years;
    while (currentJd < maxJd) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04d-01-01", boundaryYear);
        std::string nextIso = buf;
        double nextJd = 0.0;
        if (!julianDateFromIsoDate(nextIso, nextJd) || nextJd >= maxJd) {
            nextIso = options_.cadDateMax;
            nextJd = maxJd;
        }
        Window w;
        w.minIso = currentIso;
        w.maxIso = nextIso;
        w.minJd = currentJd;
        w.maxJd = nextJd;
        windows.push_back(w);
        currentIso = nextIso;
        currentJd = nextJd;
        boundaryYear += years;
    }
    return windows;
}

bool Ingestor::fetchApproaches(std::vector<ParsedApproach>& rows, IngestReport& report) {
    std::string error;
    const std::vector<Window> initial = makeWindows(error);
    if (initial.empty()) {
        report.error = error.empty() ? "no CAD windows to fetch" : error;
        return false;
    }

    // Count first: one request for the whole range, before any bulk download.
    const Fetcher::Result count = fetcher_.get(cadCountUrl(), resumeCadStartJd_ > 0.0);
    if (!count.ok) {
        report.error = "CAD count request failed: " + count.error;
        return false;
    }
    {
        ValidationReport probe;
        std::vector<ParsedApproach> none;
        const ParseStatus status = parseCadApproaches(count.body, none, probe);
        if (!status) {
            report.error = status.error;
            return false;
        }
        // A total-only response carries the number in "total" and sets
        // "count" to 0 (it returns no rows), so "total" wins when present.
        report.cadTotalReported = probe.declaredTotal.value_or(probe.declaredCount);
    }
    logLine("CAD: " + std::to_string(report.cadTotalReported) + " close approaches match " + options_.cadDateMin +
            ".." + options_.cadDateMax + " within " + num(options_.cadDistMaxAU, 6) + " AU");
    if (report.cadTotalReported > options_.confirmThreshold && !options_.assumeYes) {
        report.error = "CAD would download " + std::to_string(report.cadTotalReported) + " rows, above the " +
                       std::to_string(options_.confirmThreshold) +
                       " row threshold. Re-run with --yes to accept, or narrow --date-min/--date-max/--dist-max.";
        return false;
    }

    std::deque<Window> pending(initial.begin(), initial.end());
    while (!pending.empty()) {
        const Window w = pending.front();
        pending.pop_front();

        const bool alreadyDone = progressLoaded_ && w.maxJd <= resumeCadStartJd_;
        const Fetcher::Result page = fetcher_.get(cadWindowUrl(w.minIso, w.maxIso), alreadyDone);
        if (!page.ok) {
            report.error = "CAD window " + w.minIso + ".." + w.maxIso + " failed: " + page.error;
            return false;
        }
        ValidationReport windowReport;
        std::vector<ParsedApproach> windowRows;
        const ParseStatus status = parseCadApproaches(page.body, windowRows, windowReport);
        if (!status) {
            report.error = status.error;
            return false;
        }

        // The response states how many rows exist in total for this window. If
        // that is more than it returned, the window is too wide: halve it and
        // try again. Splitting by time avoids CAD's offset paging entirely,
        // which matters because limit-from is documented differently there
        // (1-based) than in SBDB (0-based).
        const std::size_t total = windowReport.declaredTotal.value_or(windowReport.rowsSeen);
        if (total > windowReport.rowsSeen) {
            const double midJd = std::floor((w.minJd + w.maxJd) * 0.5) + 0.5;
            if (midJd <= w.minJd || midJd >= w.maxJd) {
                report.error = "CAD window " + w.minIso + ".." + w.maxIso + " holds " + std::to_string(total) +
                               " rows but cannot be split further; raise --page-size";
                return false;
            }
            Window left = w, right = w;
            left.maxJd = midJd;
            left.maxIso = formatJulianDay(midJd);
            right.minJd = midJd;
            right.minIso = left.maxIso;
            pending.push_front(right);
            pending.push_front(left);
            ++report.cadWindowSplits;
            logLine("CAD: window " + w.minIso + ".." + w.maxIso + " has " + std::to_string(total) +
                    " rows, splitting at " + left.maxIso);
            continue;
        }

        rows.insert(rows.end(), std::make_move_iterator(windowRows.begin()),
                    std::make_move_iterator(windowRows.end()));
        report.cad.merge(windowReport);
        ++report.cadWindows;
        logLine("CAD: " + w.minIso + ".." + w.maxIso + " -> " + std::to_string(windowReport.rowsAccepted) +
                " rows (total " + std::to_string(rows.size()) + ")");

        if (w.maxJd > resumeCadStartJd_) {
            resumeCadStartJd_ = w.maxJd;
            saveProgress();
        }
    }
    return true;
}

// --- run -------------------------------------------------------------------

bool Ingestor::run(Dataset& out, IngestReport& report) {
    const auto started = std::chrono::steady_clock::now();
    report.startedUtc = utcNowIso();
    report.mode = fetcher_.mode() == CacheMode::Offline ? "offline"
                  : fetcher_.mode() == CacheMode::Refresh ? "refresh"
                                                          : "online";
    report.cadDateMin = options_.cadDateMin;
    report.cadDateMax = options_.cadDateMax;
    report.cadDistMaxAU = options_.cadDistMaxAU;
    report.cadWindowYears = options_.cadWindowYears;
    report.sbdbPageSize = options_.sbdbPageSize;
    report.cadPageSize = options_.cadPageSize;

    loadProgress();

    bool ok = true;
    std::vector<Asteroid> objects;
    std::vector<ParsedApproach> rows;
    if (ok && !options_.skipSbdb) {
        ok = fetchObjects(objects, report);
    }
    if (ok && !options_.skipCad) {
        ok = fetchApproaches(rows, report);
    }

    if (ok) {
        // Window edges are inclusive at both ends, so a row landing exactly on
        // a boundary can be returned by two neighbouring windows. Drop exact
        // repeats (same designation, same instant) and count them.
        std::sort(rows.begin(), rows.end(), [](const ParsedApproach& a, const ParsedApproach& b) {
            if (a.designation != b.designation) {
                return a.designation < b.designation;
            }
            return a.approach.jdTdb < b.approach.jdTdb;
        });
        const auto last = std::unique(rows.begin(), rows.end(), [](const ParsedApproach& a, const ParsedApproach& b) {
            return a.designation == b.designation && a.approach.jdTdb == b.approach.jdTdb;
        });
        report.duplicateRowsDropped = static_cast<std::size_t>(std::distance(last, rows.end()));
        rows.erase(last, rows.end());

        // With --skip-sbdb the caller supplies the objects (from neo.db, or from
        // an earlier run), so the dataset must be left alone: replacing it with
        // the empty vector would silently make every CAD row unmatched.
        if (!options_.skipSbdb) {
            out.setObjects(std::move(objects));
        }
        report.join = out.joinApproaches(std::move(rows));

        report.objects = out.objectCount();
        report.approaches = out.approachCount();
        for (std::uint32_t i = 0; i < out.records().size(); ++i) {
            const AsteroidRecord& record = out.records()[i];
            if (record.approachCount == 0) {
                ++report.objectsWithoutApproaches;
            }
            if (!record.object.orbital.propagationSupported()) {
                ++report.unpropagatable;
            }
            if (record.object.physical.diameterKm) {
                ++report.measuredDiameters;
            } else if (record.object.physical.estimatedDiameterKm()) {
                ++report.estimatedDiameters;
            } else {
                ++report.noDiameterAtAll;
            }
        }
    }

    report.fetch = fetcher_.stats();
    report.finishedUtc = utcNowIso();
    report.elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    report.ok = ok;
    return ok;
}

} // namespace neo
