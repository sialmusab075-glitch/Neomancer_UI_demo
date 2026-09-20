// Ingest pipeline tests: cache, politeness, retry/backoff, SBDB paging, CAD
// window splitting, resume after an interrupted run, and the report.
//
// No network. A FakeHttpClient serves the saved fixtures (and scripted
// failures) for the exact URLs the Ingestor builds, counts every request, and
// the Fetcher's sleep is replaced with a no-op so backoff costs no wall time.

#include "neo/ingest/Fetcher.h"
#include "neo/ingest/Ingestor.h"
#include "neo/ingest/ResponseCache.h"
#include "neo/model/Dataset.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_fixtureDir;
std::filesystem::path g_tempRoot;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

std::string readFixture(const char* name) {
    std::ifstream in(g_fixtureDir + "/" + name, std::ios::binary);
    if (!in) {
        std::printf("  FAIL  cannot open fixture %s\n", name);
        ++g_failures;
        return std::string();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string countPayload(const char* version, std::size_t count) {
    return std::string(R"({"signature":{"version":")") + version + R"(","source":"fake"},"count":)" +
           std::to_string(count) + "}";
}

// What CAD really answers to total-only=true: the number is in "total" and
// "count" is 0, because the response carries no rows.
std::string cadTotalOnlyPayload(std::size_t total) {
    return std::string(R"({"signature":{"version":"1.5","source":"fake"},"total":)") + std::to_string(total) +
           R"(,"count":0})";
}

// A CAD payload with `total` larger than the rows it carries, which is how a
// real response tells us the window is too wide.
std::string cadOversizedPayload(std::size_t total) {
    return std::string(R"({"signature":{"version":"1.5","source":"fake"},"count":1,"total":)") +
           std::to_string(total) +
           R"(,"fields":["des","jd","dist","v_rel"],"data":[["2020 AA","2458849.5","0.01","12.5"]]})";
}

std::string cadRowsPayload(const std::vector<std::string>& designations, double baseJd) {
    std::string data;
    for (std::size_t i = 0; i < designations.size(); ++i) {
        if (i > 0) {
            data += ",";
        }
        data += "[\"" + designations[i] + "\",\"" + std::to_string(baseJd + static_cast<double>(i)) +
                "\",\"0.01\",\"12.5\"]";
    }
    return std::string(R"({"signature":{"version":"1.5","source":"fake"},"count":)" +
                       std::to_string(designations.size()) + R"(,"total":)" + std::to_string(designations.size()) +
                       R"(,"fields":["des","jd","dist","v_rel"],"data":[)" + data + "]}");
}

// --- fake transport ---------------------------------------------------------

class FakeHttpClient : public neo::IHttpClient {
public:
    // Response served for a URL, plus optional scripted failures before it.
    struct Entry {
        std::string body;
        int         failuresFirst = 0; // this many 502s before the body
        int         status = 200;
    };

    void serve(const std::string& url, std::string body, int failuresFirst = 0) {
        Entry entry;
        entry.body = std::move(body);
        entry.failuresFirst = failuresFirst;
        entries_[url] = entry;
    }
    // Any request for this URL fails as if the process had no network.
    void breakUrl(const std::string& url) { broken_.insert(url); }

    neo::HttpResponse get(const std::string& url) override {
        ++requests_;
        requestedUrls_.push_back(url);
        neo::HttpResponse response;
        if (broken_.count(url) != 0) {
            response.status = 0;
            response.error = "scripted transport failure";
            return response;
        }
        const auto it = entries_.find(url);
        if (it == entries_.end()) {
            response.status = 404;
            response.body = R"({"code":"404","message":"fake: no entry for this URL"})";
            return response;
        }
        if (it->second.failuresFirst > 0) {
            --it->second.failuresFirst;
            response.status = 502;
            response.body = "<html>502 Bad Gateway</html>";
            return response;
        }
        response.status = it->second.status;
        response.body = it->second.body;
        return response;
    }
    const char* userAgent() const override { return "neo_ingest_tests/1.0"; }

    std::size_t requests() const { return requests_; }
    void resetCounters() {
        requests_ = 0;
        requestedUrls_.clear();
    }
    bool requested(const std::string& url) const {
        return std::find(requestedUrls_.begin(), requestedUrls_.end(), url) != requestedUrls_.end();
    }

private:
    std::map<std::string, Entry> entries_;
    std::set<std::string>        broken_;
    std::vector<std::string>     requestedUrls_;
    std::size_t                  requests_ = 0;
};

// A Fetcher whose waiting costs no wall-clock time (the durations are still
// accumulated, so the politeness policy stays observable).
neo::Fetcher makeFetcher(neo::IHttpClient& http, neo::ResponseCache& cache, neo::CacheMode mode,
                         neo::FetchPolicy policy = neo::FetchPolicy()) {
    neo::Fetcher fetcher(http, cache, mode, policy);
    fetcher.setSleepFunction([](std::chrono::milliseconds) {});
    return fetcher;
}

std::filesystem::path freshDir(const char* name) {
    const std::filesystem::path dir = g_tempRoot / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Options that keep a test run tiny: one SBDB page, one CAD window.
neo::IngestOptions smallOptions() {
    neo::IngestOptions options;
    options.sbdbPageSize = 5000;
    options.cadPageSize = 5000;
    options.cadDateMin = "2020-01-01";
    options.cadDateMax = "2021-01-01";
    options.cadWindowYears = 1;
    options.cadDistMaxAU = 0.05;
    return options;
}

// Serves the fixtures for whatever URLs these options produce.
void serveFixtures(FakeHttpClient& http, const neo::Ingestor& ingestor, std::size_t sbdbCount = 23) {
    http.serve(ingestor.sbdbCountUrl(), countPayload("1.0", sbdbCount));
    http.serve(ingestor.sbdbPageUrl(0), readFixture("sbdb_neo_page.json"));
    http.serve(ingestor.cadCountUrl(), countPayload("1.5", 22));
    http.serve(ingestor.cadWindowUrl("2020-01-01", "2021-01-01"), readFixture("cad_pha_window.json"));
}

// --- tests -----------------------------------------------------------------

void testCache() {
    std::printf("[cache] key, round trip, and URL verification\n");
    const std::filesystem::path dir = freshDir("cache");
    neo::ResponseCache cache(dir.string());

    const std::string url = "https://ssd-api.jpl.nasa.gov/cad.api?des=99942";
    std::string body;
    check(!cache.has(url), "a fresh cache is empty");
    check(!cache.load(url, body), "loading a missing entry fails");
    check(cache.store(url, "{\"ok\":1}", 200), "storing an entry succeeds");
    check(cache.has(url), "the entry is now present");
    check(cache.load(url, body) && body == "{\"ok\":1}", "the body round-trips byte for byte", body);
    check(neo::ResponseCache::keyFor(url) != neo::ResponseCache::keyFor(url + "&x=1"), "different URLs, different keys");
    check(neo::ResponseCache::keyFor(url).size() == 16, "the key is a 16-hex-digit hash");

    // The stored URL is checked on load, so a hand-edited or colliding entry is
    // ignored rather than served as if it were the right response.
    std::ifstream metaIn(cache.metaPath(url), std::ios::binary);
    std::string meta((std::istreambuf_iterator<char>(metaIn)), std::istreambuf_iterator<char>());
    metaIn.close();
    check(meta.find(url) != std::string::npos, "the metadata records the exact URL");
    std::ofstream metaOut(cache.metaPath(url), std::ios::binary | std::ios::trunc);
    metaOut << R"({"url":"https://example.com/other","status":200})";
    metaOut.close();
    check(!cache.load(url, body), "an entry whose stored URL differs is not served");
}

void testPoliteRetry() {
    std::printf("[fetch] retry on 5xx, give up on 4xx, respect the cache\n");
    const std::filesystem::path dir = freshDir("fetch");
    neo::ResponseCache cache(dir.string());
    FakeHttpClient http;
    const std::string url = "https://ssd-api.jpl.nasa.gov/test?a=1";
    http.serve(url, "{\"ok\":1}", 2); // two 502s, then the body

    neo::FetchPolicy policy;
    policy.maxAttempts = 5;
    neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal, policy);

    const neo::Fetcher::Result first = fetcher.get(url);
    check(first.ok && first.body == "{\"ok\":1}", "a 502-then-200 sequence succeeds", first.error);
    check(fetcher.stats().retries == 2, "both retries are counted");
    check(fetcher.stats().requests == 3, "three requests were issued");
    check(fetcher.stats().waitTime.count() > 0, "backoff waiting is accounted for");
    check(!first.fromCache, "the first fetch came from the network");

    const neo::Fetcher::Result second = fetcher.get(url);
    check(second.ok && second.fromCache, "the second fetch is a cache hit");
    check(fetcher.stats().requests == 3, "a cache hit issues no request");
    check(fetcher.stats().cacheHits == 1, "the cache hit is counted");

    // 4xx is the request's own fault: retrying it just wastes JPL's time.
    const neo::Fetcher::Result missing = fetcher.get("https://ssd-api.jpl.nasa.gov/test?missing=1");
    check(!missing.ok, "a 404 fails");
    check(fetcher.stats().requests == 4, "a 4xx is not retried");
    check(missing.error.find("404") != std::string::npos, "the error names the status", missing.error);

    // Exhausted retries report how many attempts were made.
    FakeHttpClient always502;
    always502.serve(url, "never", 99);
    neo::Fetcher tired = makeFetcher(always502, cache, neo::CacheMode::Refresh, policy);
    const neo::Fetcher::Result gaveUp = tired.get(url);
    check(!gaveUp.ok && gaveUp.error.find("5 attempts") != std::string::npos, "gives up after maxAttempts",
          gaveUp.error);
    check(tired.stats().failures == 1, "the failure is counted");
}

void testCacheModes() {
    std::printf("[fetch] offline and refresh modes\n");
    const std::filesystem::path dir = freshDir("modes");
    neo::ResponseCache cache(dir.string());
    const std::string url = "https://ssd-api.jpl.nasa.gov/test?mode=1";

    FakeHttpClient http;
    http.serve(url, "first");
    {
        neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);
        check(fetcher.get(url).ok, "populate the cache");
    }
    {
        FakeHttpClient noNetwork; // every URL would 404 if touched
        neo::Fetcher fetcher = makeFetcher(noNetwork, cache, neo::CacheMode::Offline);
        const neo::Fetcher::Result result = fetcher.get(url);
        check(result.ok && result.fromCache && result.body == "first", "offline serves the cached body");
        check(noNetwork.requests() == 0, "offline issues no requests");
        const neo::Fetcher::Result missing = fetcher.get("https://ssd-api.jpl.nasa.gov/test?absent=1");
        check(!missing.ok && missing.error.find("offline") != std::string::npos, "offline reports a cache miss",
              missing.error);
    }
    {
        http.serve(url, "second");
        neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Refresh);
        const neo::Fetcher::Result result = fetcher.get(url);
        check(result.ok && !result.fromCache && result.body == "second", "refresh ignores the cached copy");
        const neo::Fetcher::Result preferred = fetcher.get(url, true);
        check(preferred.ok && preferred.fromCache && preferred.body == "second",
              "preferCache serves the refreshed entry, so a resumed refresh does not download twice");
    }
}

void testPipeline() {
    std::printf("[ingest] full pipeline over the fixtures\n");
    const std::filesystem::path dir = freshDir("pipeline");
    neo::ResponseCache cache((dir / "cache").string());
    FakeHttpClient http;
    neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);

    neo::IngestOptions options = smallOptions();
    options.progressPath = (dir / "progress.json").string();
    neo::Ingestor ingestor(fetcher, options);
    serveFixtures(http, ingestor);

    neo::Dataset dataset;
    neo::IngestReport report;
    const bool ok = ingestor.run(dataset, report);
    check(ok, "the run succeeds", report.error);
    check(report.sbdbTotalReported == 23, "the SBDB count request is made first");
    check(report.cadTotalReported == 22, "the CAD count request is made first");
    check(report.objects == 23, "23 objects ingested");
    check(report.approaches == 19, "19 of the 22 CAD rows join",
          std::to_string(report.approaches));
    check(report.join.rowsUnmatched == 3, "3 rows have no parent object");
    check(report.cadWindows == 1 && report.cadWindowSplits == 0, "one window, no split needed");
    check(report.unpropagatable == 1, "the hyperbolic object is counted as unpropagatable");
    check(report.measuredDiameters == 8, "measured diameters counted",
          std::to_string(report.measuredDiameters));
    check(report.objects == report.measuredDiameters + report.estimatedDiameters + report.noDiameterAtAll,
          "every object lands in exactly one diameter bucket");
    check(report.fetch.requests == 4, "four requests: two counts, one SBDB page, one CAD window",
          std::to_string(report.fetch.requests));

    // The report must be writable and mention what a marker needs.
    const std::string text = report.toText();
    check(text.find("unmatched") != std::string::npos, "the text report mentions unmatched rows");
    check(text.find("SBDB objects") != std::string::npos, "the text report has sections");
    std::string error;
    check(neo::writeIngestReports(report, dir.string(), error), "reports are written", error);
    check(std::filesystem::exists(dir / "ingest_report.json") && std::filesystem::exists(dir / "ingest_report.txt"),
          "both report files exist");
    std::ifstream in((dir / "ingest_report.json").c_str(), std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(json.find("\"rows_unmatched\": 3") != std::string::npos, "the JSON report carries the join numbers");
    check(json.find("\"bytes_downloaded\"") != std::string::npos, "the JSON report carries the fetch numbers");

    // A second run with the same cache must not touch the network at all.
    http.resetCounters();
    neo::Fetcher again = makeFetcher(http, cache, neo::CacheMode::Normal);
    neo::Ingestor secondPass(again, options);
    neo::Dataset second;
    neo::IngestReport secondReport;
    check(secondPass.run(second, secondReport), "a repeated run succeeds", secondReport.error);
    check(http.requests() == 0, "a repeated run is served entirely from the cache");
    check(second.objectCount() == dataset.objectCount() && second.approachCount() == dataset.approachCount(),
          "and produces the same dataset");
}

void testWindowSplitting() {
    std::printf("[ingest] a window carrying more rows than one page is split\n");
    const std::filesystem::path dir = freshDir("split");
    neo::ResponseCache cache((dir / "cache").string());
    FakeHttpClient http;
    neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);

    neo::IngestOptions options = smallOptions();
    options.skipSbdb = true;
    options.cadPageSize = 2;
    neo::Ingestor ingestor(fetcher, options);

    http.serve(ingestor.cadCountUrl(), countPayload("1.5", 4));
    // The whole year says "total 4" while returning 1 row -> must be split.
    http.serve(ingestor.cadWindowUrl("2020-01-01", "2021-01-01"), cadOversizedPayload(4));
    // Halves: 2020-01-01..2020-07-02 and 2020-07-02..2021-01-01.
    http.serve(ingestor.cadWindowUrl("2020-01-01", "2020-07-02"), cadRowsPayload({"433", "99942"}, 2458850.0));
    http.serve(ingestor.cadWindowUrl("2020-07-02", "2021-01-01"), cadRowsPayload({"433"}, 2459040.0));

    neo::Dataset dataset;
    neo::IngestReport report;
    std::vector<neo::Asteroid> objects(2);
    objects[0].pdes = "433";
    objects[1].pdes = "99942";
    dataset.setObjects(std::move(objects));

    const bool ok = ingestor.run(dataset, report);
    check(ok, "the split run succeeds", report.error);
    check(report.cadWindowSplits == 1, "the oversized window was split once",
          std::to_string(report.cadWindowSplits));
    check(report.cadWindows == 2, "two halves were accepted", std::to_string(report.cadWindows));
    check(report.approaches == 3, "all rows from both halves are kept", std::to_string(report.approaches));
    check(report.duplicateRowsDropped == 0, "no duplicates in this split");
}

void testDuplicateWindowEdge() {
    std::printf("[ingest] a row on a window edge is not counted twice\n");
    const std::filesystem::path dir = freshDir("dupes");
    neo::ResponseCache cache((dir / "cache").string());
    FakeHttpClient http;
    neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);

    neo::IngestOptions options = smallOptions();
    options.skipSbdb = true;
    options.cadDateMax = "2022-01-01";
    neo::Ingestor ingestor(fetcher, options);
    http.serve(ingestor.cadCountUrl(), countPayload("1.5", 2));
    // Both windows report the same encounter, as CAD does when a row sits
    // exactly on the shared boundary (date-min and date-max are inclusive).
    http.serve(ingestor.cadWindowUrl("2020-01-01", "2021-01-01"), cadRowsPayload({"433"}, 2459215.5));
    http.serve(ingestor.cadWindowUrl("2021-01-01", "2022-01-01"), cadRowsPayload({"433"}, 2459215.5));

    neo::Dataset dataset;
    std::vector<neo::Asteroid> objects(1);
    objects[0].pdes = "433";
    dataset.setObjects(std::move(objects));
    neo::IngestReport report;
    check(ingestor.run(dataset, report), "the run succeeds", report.error);
    check(report.duplicateRowsDropped == 1, "the duplicated edge row is dropped",
          std::to_string(report.duplicateRowsDropped));
    check(report.approaches == 1, "one approach is stored");
}

void testResumeAfterInterrupt() {
    std::printf("[ingest] resume after an interrupted run\n");
    const std::filesystem::path dir = freshDir("resume");
    const std::string cacheDir = (dir / "cache").string();
    const std::string progress = (dir / "progress.json").string();

    neo::IngestOptions options = smallOptions();
    options.cadDateMax = "2022-01-01"; // two windows, so one can fail
    options.progressPath = progress;

    // --- first run: the second CAD window is unreachable -------------------
    {
        neo::ResponseCache cache(cacheDir);
        FakeHttpClient http;
        neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);
        neo::Ingestor ingestor(fetcher, options);
        http.serve(ingestor.sbdbCountUrl(), countPayload("1.0", 23));
        http.serve(ingestor.sbdbPageUrl(0), readFixture("sbdb_neo_page.json"));
        http.serve(ingestor.cadCountUrl(), countPayload("1.5", 30));
        http.serve(ingestor.cadWindowUrl("2020-01-01", "2021-01-01"), readFixture("cad_pha_window.json"));
        http.breakUrl(ingestor.cadWindowUrl("2021-01-01", "2022-01-01"));

        neo::Dataset dataset;
        neo::IngestReport report;
        const bool ok = ingestor.run(dataset, report);
        check(!ok, "the interrupted run reports failure");
        check(report.error.find("2021-01-01") != std::string::npos, "the error names the window that failed",
              report.error);
        check(report.cadWindows == 1, "the first window did complete");
    }
    check(std::filesystem::exists(progress), "a progress file was written");

    // --- second run: the failing window now works. Everything completed
    // before must come from the cache, not the network.
    {
        neo::ResponseCache cache(cacheDir);
        FakeHttpClient http;
        neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);
        neo::Ingestor ingestor(fetcher, options);
        // Deliberately serve ONLY the window that had failed: if the resumed
        // run tried to re-download anything else, it would 404 and fail.
        http.serve(ingestor.cadWindowUrl("2021-01-01", "2022-01-01"), cadRowsPayload({"433", "99942"}, 2459300.0));

        neo::Dataset dataset;
        neo::IngestReport report;
        const bool ok = ingestor.run(dataset, report);
        check(ok, "the resumed run completes", report.error);
        check(http.requests() == 1, "only the missing window is downloaded",
              std::to_string(http.requests()));
        check(report.fetch.cacheHits >= 4, "everything else came from the cache",
              std::to_string(report.fetch.cacheHits));
        check(report.objects == 23, "the dataset is complete after resuming");
        check(report.approaches == 21, "rows from both windows are present",
              std::to_string(report.approaches));
    }

    // A progress file from different parameters must be ignored, not misapplied.
    {
        neo::ResponseCache cache(cacheDir);
        FakeHttpClient http;
        neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);
        neo::IngestOptions other = options;
        other.cadDistMaxAU = 0.2; // different request set
        other.skipSbdb = true;
        neo::Ingestor ingestor(fetcher, other);
        http.serve(ingestor.cadCountUrl(), countPayload("1.5", 1));
        http.serve(ingestor.cadWindowUrl("2020-01-01", "2021-01-01"), cadRowsPayload({"433"}, 2459000.0));
        http.serve(ingestor.cadWindowUrl("2021-01-01", "2022-01-01"), cadRowsPayload({}, 2459400.0));
        neo::Dataset dataset;
        std::vector<neo::Asteroid> objects(1);
        objects[0].pdes = "433";
        dataset.setObjects(std::move(objects));
        neo::IngestReport report;
        check(ingestor.run(dataset, report), "a run with different parameters starts fresh", report.error);
        check(http.requests() == 3, "and fetches its own URLs", std::to_string(http.requests()));
    }
}

void testFailuresAreFatal() {
    std::printf("[ingest] version mismatch and the row threshold stop the run\n");
    const std::filesystem::path dir = freshDir("fatal");
    neo::ResponseCache cache((dir / "cache").string());

    {
        FakeHttpClient http;
        neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);
        neo::IngestOptions options = smallOptions();
        options.skipCad = true;
        neo::Ingestor ingestor(fetcher, options);
        http.serve(ingestor.sbdbCountUrl(), countPayload("1.0", 23));
        http.serve(ingestor.sbdbPageUrl(0), readFixture("sbdb_bad_signature.json"));
        neo::Dataset dataset;
        neo::IngestReport report;
        check(!ingestor.run(dataset, report), "an unexpected signature version fails the run");
        check(report.error.find("9.9") != std::string::npos, "the error names the version", report.error);
    }
    {
        FakeHttpClient http;
        neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);
        neo::IngestOptions options = smallOptions();
        options.skipSbdb = true;
        options.confirmThreshold = 10;
        neo::Ingestor ingestor(fetcher, options);
        // The real total-only shape: the number lives in "total", not "count".
        http.serve(ingestor.cadCountUrl(), cadTotalOnlyPayload(5000));
        neo::Dataset dataset;
        neo::IngestReport report;
        check(!ingestor.run(dataset, report), "a row count above the threshold stops the run");
        check(report.cadTotalReported == 5000, "the count comes from 'total' in a total-only response",
              std::to_string(report.cadTotalReported));
        check(report.error.find("--yes") != std::string::npos, "the error says how to proceed", report.error);
        check(report.cadWindows == 0, "nothing was downloaded");

        options.assumeYes = true;
        neo::Ingestor confirmed(fetcher, options);
        http.serve(confirmed.cadWindowUrl("2020-01-01", "2021-01-01"), cadRowsPayload({"433"}, 2459000.0));
        neo::Dataset dataset2;
        std::vector<neo::Asteroid> objects(1);
        objects[0].pdes = "433";
        dataset2.setObjects(std::move(objects));
        neo::IngestReport report2;
        check(confirmed.run(dataset2, report2), "--yes proceeds past the threshold", report2.error);
    }
}

void testUrls() {
    std::printf("[ingest] request URLs carry the documented parameters\n");
    const std::filesystem::path dir = freshDir("urls");
    neo::ResponseCache cache(dir.string());
    FakeHttpClient http;
    neo::Fetcher fetcher = makeFetcher(http, cache, neo::CacheMode::Normal);
    neo::IngestOptions options; // the shipped defaults
    neo::Ingestor ingestor(fetcher, options);

    const std::string page = ingestor.sbdbPageUrl(5000);
    check(page.find("sb-group=neo") != std::string::npos, "SBDB asks for the NEO group");
    check(page.find("full-prec=1") != std::string::npos, "SBDB asks for full precision");
    check(page.find(",n,") != std::string::npos, "SBDB requests the mean motion 'n'");
    check(page.find("epoch") != std::string::npos, "SBDB requests the epoch");
    check(page.find("limit=5000&limit-from=5000") != std::string::npos, "SBDB pages with limit/limit-from", page);
    check(ingestor.sbdbCountUrl().find("fields=") == std::string::npos,
          "the count request asks for no fields, so SBDB answers with a count");

    const std::string window = ingestor.cadWindowUrl("1950-01-01", "1955-01-01");
    check(window.find("date-min=1950-01-01&date-max=1955-01-01") != std::string::npos, "CAD window dates", window);
    check(window.find("dist-max=0.05") != std::string::npos, "CAD default distance is 0.05 AU", window);
    check(window.find("diameter=true") != std::string::npos && window.find("fullname=true") != std::string::npos,
          "CAD asks for the optional diameter and fullname columns");
    check(window.find("neo=true") != std::string::npos, "CAD is limited to NEOs, matching the SBDB group");
    check(ingestor.cadCountUrl().find("total-only=true") != std::string::npos, "the CAD count request is total-only");
    check(options.cadDateMin == "1950-01-01" && options.cadDateMax == "2150-01-01",
          "the shipped CAD window is 1950..2150");
}

} // namespace

int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : NEO_FIXTURES_DIR;
    g_tempRoot = std::filesystem::temp_directory_path() / "neo_ingest_tests";
    std::error_code ec;
    std::filesystem::remove_all(g_tempRoot, ec);
    std::filesystem::create_directories(g_tempRoot, ec);
    std::printf("fixtures: %s\ntemp:     %s\n", g_fixtureDir.c_str(), g_tempRoot.string().c_str());

    testCache();
    testPoliteRetry();
    testCacheModes();
    testPipeline();
    testWindowSplitting();
    testDuplicateWindowEdge();
    testResumeAfterInterrupt();
    testFailuresAreFatal();
    testUrls();

    std::filesystem::remove_all(g_tempRoot, ec);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
