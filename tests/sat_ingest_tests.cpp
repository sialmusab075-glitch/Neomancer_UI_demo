// CelesTrak ingest tests (src/sat/ingest). No network: a FakeHttpClient serves OMM JSON built from the
// real element sets in tests/fixtures/sat. Where a test needs a satellite that is not in the fixtures
// (a "Hubble"), it RELABELS a fixture set; that is test data for exercising the pinned-object logic, and
// it is not Hubble's real orbit.
//
// CelesTrak itself has never been contacted from the build environment. These tests prove the plumbing
// (URLs, caching, retry, the two-hour refresh rule, merging, the pinned-object check); the first live run
// of sat_ingest is the first contact with the real service.

#include "sat/ingest/CelesTrakIngestor.h"
#include "sat/parse/TleParser.h"
#include "sat/store/Catalog.h"

#include "neo/ingest/Fetcher.h"
#include "neo/ingest/HttpClient.h"
#include "neo/ingest/ResponseCache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef SAT_FIXTURES_DIR
#define SAT_FIXTURES_DIR "tests/fixtures/sat"
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

class FakeHttpClient : public neo::IHttpClient {
public:
    void serve(const std::string& url, std::string body, int status = 200) { entries_[url] = {std::move(body), status}; }
    void failAlways(const std::string& url, int status) { failing_[url] = status; }

    neo::HttpResponse get(const std::string& url) override {
        ++requests_;
        requested_.push_back(url);
        neo::HttpResponse response;
        const auto fail = failing_.find(url);
        if (fail != failing_.end()) {
            response.status = fail->second;
            response.body = "<html>gateway</html>";
            return response;
        }
        const auto it = entries_.find(url);
        if (it == entries_.end()) {
            response.status = 404;
            response.body = "No GP data found";
            return response;
        }
        response.status = it->second.second;
        response.body = it->second.first;
        return response;
    }
    const char* userAgent() const override { return "sat_ingest_tests/1.0"; }

    std::size_t requests() const { return requests_; }
    bool requested(const std::string& url) const { return std::find(requested_.begin(), requested_.end(), url) != requested_.end(); }
    void reset() {
        requests_ = 0;
        requested_.clear();
    }

private:
    std::map<std::string, std::pair<std::string, int>> entries_;
    std::map<std::string, int>                         failing_;
    std::vector<std::string>                           requested_;
    std::size_t                                        requests_ = 0;
};

struct Rig {
    std::filesystem::path dir;
    FakeHttpClient        http;
    neo::ResponseCache    cache;
    Rig(const std::string& name)
        : dir(std::filesystem::temp_directory_path() / ("solsim_sat_ingest_" + name)), cache(dir.string()) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    ~Rig() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    neo::Fetcher makeFetcher(neo::CacheMode mode, int attempts = 3) {
        neo::FetchPolicy policy;
        policy.minInterval = std::chrono::milliseconds(0);
        policy.maxAttempts = attempts;
        neo::Fetcher fetcher(http, cache, mode, policy);
        fetcher.setSleepFunction([](std::chrono::milliseconds) {});
        return fetcher;
    }
};

struct Samples {
    sat::ElementSet iss, vanguard, mario;
};

Samples loadSamples() {
    std::vector<sat::ElementSet> sets;
    sat::parseTleText(readFile(std::string(SAT_FIXTURES_DIR) + "/celestrak_3le_sample.txt"), sets);
    Samples s;
    if (sets.size() == 3) {
        s.iss = sets[0];
        s.vanguard = sets[1];
        s.mario = sets[2];
    }
    check(sets.size() == 3, "the three real fixture sets are available");
    return s;
}

const std::string kStations = sat::celesTrakGroupUrl("stations");
const std::string kVisual = sat::celesTrakGroupUrl("visual");

void testUrls() {
    std::printf("[urls] request construction\n");
    check(sat::celesTrakGroupUrl("stations") == "https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=json",
          "the group URL", sat::celesTrakGroupUrl("stations"));
    check(sat::celesTrakCatalogNumberUrl(25544) == "https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=json", "the catalogue-number URL");
    check(sat::validGroupName("stations") && sat::validGroupName("visual") && sat::validGroupName("gps-ops") && sat::validGroupName("starlink"),
          "ordinary group names are valid");
    check(!sat::validGroupName("") && !sat::validGroupName("Stations") && !sat::validGroupName("a&b=c") && !sat::validGroupName("x y") &&
              !sat::validGroupName("../x") && !sat::validGroupName(std::string(41, 'a')),
          "empty, upper-case, URL-special, spaced, path-like and over-long names are refused");

    const sat::SatIngestOptions defaults;
    check(defaults.groups == std::vector<std::string>({"stations", "visual"}), "the default groups are stations and visual");
    std::set<std::uint32_t> pinned;
    for (const sat::PinnedObject& p : defaults.pinned) {
        pinned.insert(p.noradId);
    }
    check(pinned == std::set<std::uint32_t>({25544, 20580, 48274}), "the default pinned objects are the ISS, Hubble and the Tiangong core module");
    check(defaults.minRefreshInterval == std::chrono::minutes(120), "the refresh interval is CelesTrak's two hours");
}

void testHappyPath() {
    std::printf("[ingest] groups, pinned objects, merging, caching\n");
    const Samples s = loadSamples();
    Rig rig("happy");
    // stations: the ISS and MARIO. visual: Vanguard, plus a NEWER epoch of MARIO (the same satellite twice).
    sat::ElementSet marioNewer = s.mario;
    marioNewer.epochJdWhole += 1.0;
    sat::ElementSet hst = s.vanguard; // TEST DATA: Vanguard's elements under Hubble's catalogue number and name
    hst.noradId = 20580;
    hst.name = "HST";
    rig.http.serve(kStations, sat::toOmmJson({s.iss, s.mario}));
    rig.http.serve(kVisual, sat::toOmmJson({s.vanguard, marioNewer}));
    rig.http.serve(sat::celesTrakCatalogNumberUrl(20580), sat::toOmmJson({hst}));
    // 48274 is not served: the fake answers 404 "No GP data found"

    neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal);
    sat::CelesTrakIngestor ingestor(fetcher, rig.cache, sat::SatIngestOptions());
    std::vector<sat::ElementSet> out;
    sat::SatIngestReport report;
    check(ingestor.run(out, report), "the ingest succeeds", report.error);
    check(rig.http.requests() == 4, "4 requests: two groups, then Hubble and Tiangong (the ISS was already in 'stations')", std::to_string(rig.http.requests()));
    check(!rig.http.requested(sat::celesTrakCatalogNumberUrl(25544)), "the ISS was not fetched separately");
    check(report.accepted == 5 && report.unique == 4 && report.olderEpochsDropped == 1 && out.size() == 4,
          "5 sets accepted (2 + 2 + 1), 4 satellites, 1 duplicate dropped (MARIO appears in both groups)",
          std::to_string(report.accepted) + "/" + std::to_string(report.unique) + "/" + std::to_string(report.olderEpochsDropped));
    std::vector<std::uint32_t> ids;
    for (const sat::ElementSet& e : out) {
        ids.push_back(e.noradId);
    }
    check(ids == std::vector<std::uint32_t>({5, 20580, 25544, 55123}), "the result is sorted by catalogue number");
    for (const sat::ElementSet& e : out) {
        if (e.noradId == 55123) {
            check(e.epochJdWhole == marioNewer.epochJdWhole, "of the two MARIO sets the newer epoch was kept");
        }
    }
    check(report.sources.size() == 4 && report.sources[0].ok && report.sources[0].parse.accepted == 2 && !report.sources[0].fromCache,
          "each source is reported with its own count");
    check(report.sources[3].ok == false && report.sources[3].label.find("48274") != std::string::npos,
          "the unreachable pinned object is a failed source, not a failed run");

    bool issOk = false, hubbleOk = false, tianOk = true;
    for (const sat::PinReport& p : report.pins) {
        if (p.pinned.noradId == 25544) {
            issOk = p.found && p.nameMatches && p.foundName == "ISS (ZARYA)";
        } else if (p.pinned.noradId == 20580) {
            hubbleOk = p.found && p.nameMatches;
        } else if (p.pinned.noradId == 48274) {
            tianOk = !p.found;
        }
    }
    check(issOk && hubbleOk && tianOk, "pinned: the ISS and 'HST' found with matching names, the Tiangong core module not found");
    const std::string text = report.toText();
    check(text.find("pinned Tiangong") != std::string::npos && text.find("NOT FOUND") != std::string::npos && text.find("[network]") != std::string::npos,
          "the text report says what was and was not found");

    // Second run: everything from the cache, nothing asked of CelesTrak.
    rig.http.reset();
    neo::Fetcher again = rig.makeFetcher(neo::CacheMode::Normal);
    sat::CelesTrakIngestor second(again, rig.cache, sat::SatIngestOptions());
    std::vector<sat::ElementSet> out2;
    sat::SatIngestReport report2;
    check(second.run(out2, report2) && out2.size() == out.size(), "a second run gives the same satellites");
    check(rig.http.requests() == 1 && rig.http.requested(sat::celesTrakCatalogNumberUrl(48274)),
          "and asks for exactly one thing: the pinned object that failed last time (failures are not cached)", std::to_string(rig.http.requests()));
    bool cached = true;
    for (const sat::SourceReport& src : report2.sources) {
        cached = cached && (!src.ok || src.fromCache);
    }
    check(cached, "every successful source came from the cache");
    check(!rig.http.requested(kStations) && !rig.http.requested(kVisual) && !rig.http.requested(sat::celesTrakCatalogNumberUrl(20580)),
          "the two groups and Hubble were not requested again");
}

void testPinnedProblems() {
    std::printf("[ingest] pinned objects that are wrong or missing\n");
    const Samples s = loadSamples();
    Rig rig("pinned");
    sat::ElementSet wrong = s.vanguard; // TEST DATA: some other satellite answering to Hubble's number
    wrong.noradId = 20580;
    wrong.name = "SOME OTHER SATELLITE";
    rig.http.serve(kStations, sat::toOmmJson({s.iss}));
    rig.http.serve(kVisual, sat::toOmmJson({s.mario}));
    rig.http.serve(sat::celesTrakCatalogNumberUrl(20580), sat::toOmmJson({wrong}));
    rig.http.serve(sat::celesTrakCatalogNumberUrl(48274), "No GP data found"); // 200 but not JSON

    neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal);
    sat::CelesTrakIngestor ingestor(fetcher, rig.cache, sat::SatIngestOptions());
    std::vector<sat::ElementSet> out;
    sat::SatIngestReport report;
    check(ingestor.run(out, report), "a wrong or missing pinned object does not fail the run", report.error);
    bool mismatch = false;
    for (const sat::PinReport& p : report.pins) {
        if (p.pinned.noradId == 20580) {
            mismatch = p.found && !p.nameMatches && p.foundName == "SOME OTHER SATELLITE";
        }
    }
    check(mismatch, "a different name under a pinned number is reported as a mismatch");
    const std::string text = report.toText();
    check(text.find("DOES NOT MATCH") != std::string::npos && text.find("check the catalogue number") != std::string::npos,
          "and the report tells the user to check the number");
    check(text.find("not JSON") != std::string::npos, "a plain-text 'No GP data found' body is reported as not JSON");
    check(out.size() == 3, "the wrongly named satellite is still in the output (the user decides)", std::to_string(out.size()));

    // custom pinned objects and names
    sat::SatIngestOptions options;
    options.groups = {"stations"};
    options.pinned = {{25544, "Space station", {"zarya"}}}; // case-insensitive substring
    neo::Fetcher f2 = rig.makeFetcher(neo::CacheMode::Normal);
    sat::CelesTrakIngestor custom(f2, rig.cache, options);
    std::vector<sat::ElementSet> out2;
    sat::SatIngestReport report2;
    check(custom.run(out2, report2) && report2.pins.size() == 1 && report2.pins[0].nameMatches, "pinned-name matching is a case-insensitive substring match");
}

void testFailures() {
    std::printf("[ingest] failures\n");
    const Samples s = loadSamples();
    {
        Rig rig("fail_group");
        rig.http.failAlways(kStations, 502);
        rig.http.serve(kVisual, sat::toOmmJson({s.mario}));
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal, 2);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, sat::SatIngestOptions());
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(!ingestor.run(out, report) && !report.ok && report.error.find("stations") != std::string::npos && out.empty(),
              "a group that keeps failing fails the run and names the group", report.error);
        check(rig.http.requests() == 2, "after exactly the configured number of attempts, and 'visual' was never asked", std::to_string(rig.http.requests()));
        check(report.fetch.failures == 1 && report.fetch.retries == 1, "the report carries the fetch statistics");
        check(report.toText().find("FAILED") != std::string::npos, "and the text report says FAILED");
    }
    {
        Rig rig("fail_parse");
        rig.http.serve(kStations, "No GP data found");
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, sat::SatIngestOptions());
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(!ingestor.run(out, report) && report.error.find("not JSON") != std::string::npos, "a group answered with text is a failure, not an empty result", report.error);
    }
    {
        Rig rig("bad_group");
        sat::SatIngestOptions options;
        options.groups = {"stations", "bad&name"};
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, options);
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(!ingestor.run(out, report) && report.error.find("not a valid CelesTrak group") != std::string::npos && rig.http.requests() == 0,
              "an invalid group name is refused before anything is requested", report.error);
    }
    {
        Rig rig("empty");
        rig.http.serve(kStations, "[]");
        rig.http.serve(kVisual, "[]");
        sat::SatIngestOptions options;
        options.pinned.clear();
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, options);
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(!ingestor.run(out, report) && report.error.find("no element sets") != std::string::npos, "valid but empty answers are an error, not an empty catalogue", report.error);
    }
    {
        Rig rig("offline");
        sat::SatIngestOptions options;
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Offline);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, options);
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(!ingestor.run(out, report) && rig.http.requests() == 0, "offline mode with an empty cache fails without touching the network", report.error);
    }
}

void testRefreshRule() {
    std::printf("[ingest] CelesTrak's two-hour rule in Refresh mode\n");
    const Samples s = loadSamples();
    Rig rig("refresh");
    rig.http.serve(kStations, sat::toOmmJson({s.iss}));
    rig.http.serve(kVisual, sat::toOmmJson({s.mario}));
    sat::SatIngestOptions options;
    options.pinned.clear();

    {
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, options);
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(ingestor.run(out, report) && rig.http.requests() == 2, "the first run downloads both groups");
    }
    // 30 minutes old: --refresh keeps the cached copy
    const auto now = std::filesystem::file_time_type::clock::now();
    std::error_code ec;
    std::filesystem::last_write_time(rig.cache.bodyPath(kStations), now - std::chrono::minutes(30), ec);
    std::filesystem::last_write_time(rig.cache.bodyPath(kVisual), now - std::chrono::minutes(30), ec);
    check(!ec, "the cache files' ages can be set for the test");
    rig.http.reset();
    {
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Refresh);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, options);
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(ingestor.run(out, report) && rig.http.requests() == 0, "a refresh of 30-minute-old data makes no request", std::to_string(rig.http.requests()));
        check(report.sources.size() == 2 && report.sources[0].keptYoungCache && report.sources[1].keptYoungCache && report.sources[0].fromCache,
              "and the report says the cached copy was kept");
        check(report.toText().find("kept the cached copy") != std::string::npos, "in words");
    }
    // 3 hours old: --refresh downloads again
    std::filesystem::last_write_time(rig.cache.bodyPath(kStations), now - std::chrono::hours(3), ec);
    std::filesystem::last_write_time(rig.cache.bodyPath(kVisual), now - std::chrono::hours(3), ec);
    rig.http.reset();
    {
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Refresh);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, options);
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(ingestor.run(out, report) && rig.http.requests() == 2, "a refresh of 3-hour-old data downloads both groups again", std::to_string(rig.http.requests()));
        check(!report.sources[0].keptYoungCache && !report.sources[0].fromCache, "and does not claim to have kept anything");
    }
    // a different interval is honoured
    std::filesystem::last_write_time(rig.cache.bodyPath(kStations), now - std::chrono::minutes(90), ec);
    std::filesystem::last_write_time(rig.cache.bodyPath(kVisual), now - std::chrono::minutes(90), ec);
    rig.http.reset();
    {
        sat::SatIngestOptions short_ = options;
        short_.minRefreshInterval = std::chrono::minutes(60);
        neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Refresh);
        sat::CelesTrakIngestor ingestor(fetcher, rig.cache, short_);
        std::vector<sat::ElementSet> out;
        sat::SatIngestReport report;
        check(ingestor.run(out, report) && rig.http.requests() == 2, "with a 60-minute interval, 90-minute-old data is refreshed");
    }
}

void testEndToEndCatalog() {
    std::printf("[ingest] ingest result -> catalogue file -> propagate\n");
    const Samples s = loadSamples();
    Rig rig("e2e");
    rig.http.serve(kStations, sat::toOmmJson({s.iss}));
    rig.http.serve(kVisual, sat::toOmmJson({s.vanguard, s.mario}));
    sat::SatIngestOptions options;
    options.pinned.clear();
    neo::Fetcher fetcher = rig.makeFetcher(neo::CacheMode::Normal);
    sat::CelesTrakIngestor ingestor(fetcher, rig.cache, options);
    std::vector<sat::ElementSet> out;
    sat::SatIngestReport report;
    check(ingestor.run(out, report) && out.size() == 3, "three satellites ingested");

    sat::CatalogMeta meta;
    meta.createdUtc = report.startedUtc;
    meta.source = "CelesTrak GP (OMM JSON)";
    meta.groups = options.groups;
    const std::string path = (rig.dir / "elements.json").string();
    std::string error;
    check(sat::saveCatalog(path, out, meta, error), "saved", error);
    std::vector<sat::ElementSet> loaded;
    sat::CatalogMeta loadedMeta;
    check(sat::loadCatalog(path, loaded, loadedMeta, error) && loaded.size() == 3 && loaded[0].noradId == 5 && loaded[2].noradId == 55123 &&
              loadedMeta.groups == options.groups,
          "loaded back in catalogue-number order with its groups", error);
}

} // namespace

int main() {
    testUrls();
    testHappyPath();
    testPinnedProblems();
    testFailures();
    testRefreshRule();
    testEndToEndCatalog();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
