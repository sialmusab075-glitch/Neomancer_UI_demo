#pragma once

#include "sat/model/ElementSet.h"
#include "sat/parse/ParseReport.h"

#include "neo/ingest/Fetcher.h"
#include "neo/ingest/ResponseCache.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace sat {

// Downloads element sets from CelesTrak's GP service, as OMM JSON.
//
// This is the one place the satellite library touches src/neo: it borrows that layer's HTTP
// interface, retry/backoff and raw-response cache (neo::Fetcher, neo::ResponseCache) so a download is
// polite and replayable the same way. The dependency runs one way: nothing in src/neo knows about
// satellites. Only this target (solsim_sat_ingest) links neo; the parsers, the propagator and the
// catalogue (solsim_sat) do not.
constexpr const char* kCelesTrakGpEndpoint = "https://celestrak.org/NORAD/elements/gp.php";

// An object the display must not lose track of (the ISS, Tiangong, Hubble). It is fetched by catalogue
// number when the groups did not contain it, and the name found is checked, so a wrong number cannot
// silently show a different satellite.
struct PinnedObject {
    std::uint32_t            noradId = 0;
    std::string              label;           // "Hubble"
    std::vector<std::string> nameContainsAny; // case-insensitive; any one matching is enough
};

struct SatIngestOptions {
    std::vector<std::string> groups = {"stations", "visual"};
    std::vector<PinnedObject> pinned = {
        {25544, "ISS", {"ISS"}},
        {20580, "Hubble", {"HST", "HUBBLE"}},
        {48274, "Tiangong (Tianhe core module)", {"CSS", "TIANHE", "TIANGONG"}},
    };
    // CelesTrak asks that the same data is not downloaded more often than this (the sets are
    // regenerated about every two hours). In Refresh mode a cached copy younger than this is kept.
    std::chrono::minutes minRefreshInterval{120};
};

struct SourceReport {
    std::string    label;      // "group stations", "catalogue number 20580"
    std::string    url;
    bool           ok = false;
    bool           fromCache = false;
    bool           keptYoungCache = false; // Refresh asked for, cache younger than minRefreshInterval
    std::string    error;
    SetParseReport parse;
};

struct PinReport {
    PinnedObject pinned;
    bool         found = false;       // an element set with that catalogue number is in the result
    bool         nameMatches = false;
    std::string  foundName;
};

struct SatIngestReport {
    std::string startedUtc;
    std::vector<SourceReport> sources;
    std::vector<PinReport>    pins;
    std::size_t accepted = 0;           // element sets accepted across all sources
    std::size_t olderEpochsDropped = 0; // same satellite twice: the newer epoch wins
    std::size_t unique = 0;
    neo::FetchStats fetch;
    bool        ok = false;
    std::string error;

    std::string toText() const;
};

std::string celesTrakGroupUrl(const std::string& group);
std::string celesTrakCatalogNumberUrl(std::uint32_t noradId);
bool validGroupName(const std::string& group); // lower-case letters, digits and '-' only

class CelesTrakIngestor {
public:
    CelesTrakIngestor(neo::Fetcher& fetcher, const neo::ResponseCache& cache, SatIngestOptions options);

    // Fills `out` with one element set per satellite, sorted by catalogue number. Fails when a
    // requested group cannot be fetched or parsed; a pinned object that is missing or mismatched is
    // reported but does not fail the run.
    bool run(std::vector<ElementSet>& out, SatIngestReport& report);

private:
    bool fetchSource(const std::string& label, const std::string& url, std::vector<ElementSet>& sets, SatIngestReport& report);

    neo::Fetcher&             fetcher_;
    const neo::ResponseCache& cache_;
    SatIngestOptions          options_;
};

} // namespace sat
