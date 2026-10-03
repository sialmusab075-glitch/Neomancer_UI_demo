#include "sat/ingest/CelesTrakIngestor.h"

#include "neo/model/JulianDate.h"
#include "sat/parse/OmmJsonParser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <sstream>

namespace sat {

std::string celesTrakGroupUrl(const std::string& group) {
    return std::string(kCelesTrakGpEndpoint) + "?GROUP=" + group + "&FORMAT=json";
}

std::string celesTrakCatalogNumberUrl(std::uint32_t noradId) {
    return std::string(kCelesTrakGpEndpoint) + "?CATNR=" + std::to_string(noradId) + "&FORMAT=json";
}

bool validGroupName(const std::string& group) {
    if (group.empty() || group.size() > 40) {
        return false;
    }
    for (const char c : group) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

namespace {

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool nameMatches(const std::string& name, const std::vector<std::string>& needles) {
    const std::string haystack = upper(name);
    for (const std::string& needle : needles) {
        if (!needle.empty() && haystack.find(upper(needle)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

CelesTrakIngestor::CelesTrakIngestor(neo::Fetcher& fetcher, const neo::ResponseCache& cache, SatIngestOptions options)
    : fetcher_(fetcher), cache_(cache), options_(std::move(options)) {}

bool CelesTrakIngestor::fetchSource(const std::string& label, const std::string& url, std::vector<ElementSet>& sets,
                                    SatIngestReport& report) {
    SourceReport source;
    source.label = label;
    source.url = url;

    // In Refresh mode, a copy fetched less than minRefreshInterval ago is kept: CelesTrak blocks
    // clients that ask for unchanged data more often than that.
    bool preferCache = false;
    if (fetcher_.mode() == neo::CacheMode::Refresh && cache_.has(url)) {
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(cache_.bodyPath(url), ec);
        if (!ec) {
            const auto age = std::filesystem::file_time_type::clock::now() - written;
            if (age < options_.minRefreshInterval) {
                preferCache = true;
                source.keptYoungCache = true;
            }
        }
    }

    const neo::Fetcher::Result fetched = fetcher_.get(url, preferCache);
    source.fromCache = fetched.fromCache;
    if (!fetched.ok) {
        source.error = fetched.error;
        report.sources.push_back(std::move(source));
        return false;
    }
    std::vector<ElementSet> parsed;
    const OmmParseResult result = parseOmmJson(fetched.body, parsed);
    source.parse = result.report;
    if (!result.ok) {
        source.error = result.error;
        report.sources.push_back(std::move(source));
        return false;
    }
    source.ok = true;
    report.accepted += parsed.size();
    for (ElementSet& s : parsed) {
        sets.push_back(std::move(s));
    }
    report.sources.push_back(std::move(source));
    return true;
}

bool CelesTrakIngestor::run(std::vector<ElementSet>& out, SatIngestReport& report) {
    report = SatIngestReport();
    report.startedUtc = neo::utcNowIso();
    std::vector<ElementSet> all;

    // Refuse a bad group name before anything is requested, not half way through.
    for (const std::string& group : options_.groups) {
        if (!validGroupName(group)) {
            report.error = "'" + group + "' is not a valid CelesTrak group name (lower-case letters, digits, '-')";
            report.fetch = fetcher_.stats();
            return false;
        }
    }
    for (const std::string& group : options_.groups) {
        if (!fetchSource("group " + group, celesTrakGroupUrl(group), all, report)) {
            report.error = "could not get group '" + group + "': " + report.sources.back().error;
            report.fetch = fetcher_.stats();
            return false;
        }
    }

    // Pinned objects the groups did not contain are fetched by catalogue number.
    for (const PinnedObject& pin : options_.pinned) {
        const bool present = std::any_of(all.begin(), all.end(), [&pin](const ElementSet& s) { return s.noradId == pin.noradId; });
        if (!present) {
            fetchSource("catalogue number " + std::to_string(pin.noradId) + " (" + pin.label + ")",
                        celesTrakCatalogNumberUrl(pin.noradId), all, report); // not fatal
        }
    }

    // One set per satellite: the newest epoch wins.
    std::map<std::uint32_t, std::size_t> byId;
    std::vector<ElementSet> unique;
    for (ElementSet& s : all) {
        const auto it = byId.find(s.noradId);
        if (it == byId.end()) {
            byId[s.noradId] = unique.size();
            unique.push_back(std::move(s));
        } else {
            ++report.olderEpochsDropped;
            if (s.epochJd() > unique[it->second].epochJd()) {
                unique[it->second] = std::move(s);
            }
        }
    }
    std::sort(unique.begin(), unique.end(), [](const ElementSet& a, const ElementSet& b) { return a.noradId < b.noradId; });
    report.unique = unique.size();

    for (const PinnedObject& pin : options_.pinned) {
        PinReport pr;
        pr.pinned = pin;
        for (const ElementSet& s : unique) {
            if (s.noradId == pin.noradId) {
                pr.found = true;
                pr.foundName = s.name;
                pr.nameMatches = nameMatches(s.name, pin.nameContainsAny);
                break;
            }
        }
        report.pins.push_back(std::move(pr));
    }

    report.fetch = fetcher_.stats();
    if (unique.empty()) {
        report.error = "no element sets were obtained";
        return false;
    }
    out = std::move(unique);
    report.ok = true;
    return true;
}

std::string SatIngestReport::toText() const {
    std::ostringstream o;
    o << "satellite ingest, started " << startedUtc << "\n";
    for (const SourceReport& s : sources) {
        o << "  " << s.label << ": ";
        if (!s.ok) {
            o << "FAILED (" << s.error << ")\n";
            continue;
        }
        o << s.parse.accepted << " accepted";
        if (s.parse.rejected > 0) {
            o << ", " << s.parse.rejected << " rejected";
        }
        o << (s.fromCache ? " [cache]" : " [network]");
        if (s.keptYoungCache) {
            o << " (kept the cached copy: younger than the minimum refresh interval)";
        }
        o << "\n";
        for (const auto& sample : s.parse.rejectedSamples) {
            o << "      rejected #" << sample.first << ": " << sample.second << "\n";
        }
    }
    o << "  " << accepted << " element sets accepted, " << olderEpochsDropped << " older duplicates dropped, " << unique
      << " satellites\n";
    for (const PinReport& p : pins) {
        o << "  pinned " << p.pinned.label << " (" << p.pinned.noradId << "): ";
        if (!p.found) {
            o << "NOT FOUND\n";
        } else if (!p.nameMatches) {
            o << "FOUND BUT THE NAME DOES NOT MATCH ('" << p.foundName << "'): check the catalogue number\n";
        } else {
            o << "ok ('" << p.foundName << "')\n";
        }
    }
    o << "  requests " << fetch.requests << ", cache hits " << fetch.cacheHits << ", retries " << fetch.retries << ", failures "
      << fetch.failures << "\n";
    if (!ok) {
        o << "  FAILED: " << error << "\n";
    }
    return o.str();
}

} // namespace sat
