#include "neo/ingest/Ingestor.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace neo {

namespace {

nlohmann::json validationToJson(const ValidationReport& v) {
    nlohmann::json j;
    j["signature_source"] = v.signatureSource;
    j["signature_version"] = v.signatureVersion;
    j["rows_seen"] = v.rowsSeen;
    j["rows_accepted"] = v.rowsAccepted;
    j["rows_rejected"] = v.rowsRejected;
    j["derived_distance_ranges"] = v.derivedDistanceRanges;
    nlohmann::json nulls = nlohmann::json::object();
    for (const auto& entry : v.nullCounts) {
        nulls[entry.first] = entry.second;
    }
    j["null_counts"] = nulls;
    nlohmann::json rejected = nlohmann::json::array();
    for (const RejectedRow& r : v.rejectedSamples) {
        rejected.push_back({{"row", r.index}, {"reason", r.reason}});
    }
    j["rejected_samples"] = rejected;
    return j;
}

std::string section(const char* title) { return std::string("\n") + title + "\n" + std::string(72, '-') + "\n"; }

} // namespace

std::string IngestReport::toJson() const {
    nlohmann::json j;
    j["run"] = {{"started_utc", startedUtc},
                {"finished_utc", finishedUtc},
                {"elapsed_seconds", elapsedSeconds},
                {"mode", mode},
                {"ok", ok},
                {"error", error}};
    j["request"] = {{"cad_date_min", cadDateMin},
                    {"cad_date_max", cadDateMax},
                    {"cad_dist_max_au", cadDistMaxAU},
                    {"cad_window_years", cadWindowYears},
                    {"sbdb_page_size", sbdbPageSize},
                    {"cad_page_size", cadPageSize}};
    j["sbdb"] = validationToJson(sbdb);
    j["sbdb"]["total_reported"] = sbdbTotalReported;
    j["sbdb"]["pages"] = sbdbPages;
    j["cad"] = validationToJson(cad);
    j["cad"]["total_reported"] = cadTotalReported;
    j["cad"]["windows"] = cadWindows;
    j["cad"]["window_splits"] = cadWindowSplits;
    j["cad"]["duplicate_rows_dropped"] = duplicateRowsDropped;

    nlohmann::json unmatched = nlohmann::json::array();
    for (const auto& entry : join.unmatchedSamples) {
        unmatched.push_back({{"designation", entry.first}, {"rows", entry.second}});
    }
    j["join"] = {{"rows_seen", join.rowsSeen},
                 {"rows_matched", join.rowsMatched},
                 {"rows_unmatched", join.rowsUnmatched},
                 {"distinct_unmatched", join.distinctUnmatched},
                 {"objects_with_approaches", join.objectsWithApproaches},
                 {"unmatched_samples", unmatched}};

    j["dataset"] = {{"objects", objects},
                    {"approaches", approaches},
                    {"objects_without_approaches", objectsWithoutApproaches},
                    {"grazing_or_impact", grazingOrImpact},
                    {"unpropagatable_e_ge_1", unpropagatable},
                    {"measured_diameters", measuredDiameters},
                    {"estimated_diameters", estimatedDiameters},
                    {"no_diameter", noDiameterAtAll}};

    j["database"] = {{"path", dbPath}, {"bytes", dbBytes}, {"write_seconds", dbWriteSeconds}};
    j["fetch"] = {{"requests", fetch.requests},
                  {"cache_hits", fetch.cacheHits},
                  {"retries", fetch.retries},
                  {"failures", fetch.failures},
                  {"bytes_downloaded", fetch.bytesDownloaded},
                  {"bytes_from_cache", fetch.bytesFromCache},
                  {"network_seconds", static_cast<double>(fetch.networkTime.count()) / 1000.0},
                  {"wait_seconds", static_cast<double>(fetch.waitTime.count()) / 1000.0}};
    return j.dump(2);
}

std::string IngestReport::toText() const {
    char buf[512];
    std::string out = "NEO ingest report\n";
    out += std::string(72, '=') + "\n";
    std::snprintf(buf, sizeof buf, "started   %s\nfinished  %s (%.1f s, mode %s)\nresult    %s%s\n",
                  startedUtc.c_str(), finishedUtc.c_str(), elapsedSeconds, mode.c_str(), ok ? "OK" : "FAILED: ",
                  ok ? "" : error.c_str());
    out += buf;
    std::snprintf(buf, sizeof buf, "request   CAD %s..%s within %.6f AU, %d-year windows, pages %zu/%zu\n",
                  cadDateMin.c_str(), cadDateMax.c_str(), cadDistMaxAU, cadWindowYears, sbdbPageSize, cadPageSize);
    out += buf;

    out += section("SBDB objects");
    std::snprintf(buf, sizeof buf, "reported total %zu, fetched in %zu page(s)\n", sbdbTotalReported, sbdbPages);
    out += buf;
    out += sbdb.toString() + "\n";

    out += section("CAD close approaches");
    std::snprintf(buf, sizeof buf, "reported total %zu, %zu window(s), %zu split(s), %zu duplicate row(s) dropped\n",
                  cadTotalReported, cadWindows, cadWindowSplits, duplicateRowsDropped);
    out += buf;
    out += cad.toString() + "\n";

    out += section("Join (CAD des -> SBDB pdes)");
    out += join.toString() + "\n";

    out += section("Dataset");
    std::snprintf(buf, sizeof buf,
                  "objects                 %zu\n"
                  "approaches              %zu\n"
                  "objects with none       %zu\n"
                  "closer than 1 R_Earth   %zu\n"
                  "e >= 1 (no propagation) %zu\n"
                  "measured diameters      %zu\n"
                  "H-estimated diameters   %zu\n"
                  "no diameter at all      %zu\n",
                  objects, approaches, objectsWithoutApproaches, grazingOrImpact, unpropagatable, measuredDiameters,
                  estimatedDiameters, noDiameterAtAll);
    out += buf;

    if (!dbPath.empty()) {
        out += section("Database");
        std::snprintf(buf, sizeof buf, "%s\n%.2f MB written in %.2f s\n", dbPath.c_str(),
                      static_cast<double>(dbBytes) / (1024.0 * 1024.0), dbWriteSeconds);
        out += buf;
    }

    out += section("Fetch");
    std::snprintf(buf, sizeof buf,
                  "requests %zu, cache hits %zu, retries %zu, failures %zu\n"
                  "downloaded %.2f MB, from cache %.2f MB\n"
                  "network %.1f s, polite waiting %.1f s\n",
                  fetch.requests, fetch.cacheHits, fetch.retries, fetch.failures,
                  static_cast<double>(fetch.bytesDownloaded) / (1024.0 * 1024.0),
                  static_cast<double>(fetch.bytesFromCache) / (1024.0 * 1024.0),
                  static_cast<double>(fetch.networkTime.count()) / 1000.0,
                  static_cast<double>(fetch.waitTime.count()) / 1000.0);
    out += buf;
    return out;
}

bool writeIngestReports(const IngestReport& report, const std::string& dir, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir + ": " + ec.message();
        return false;
    }
    const std::pair<std::string, std::string> files[] = {{dir + "/ingest_report.json", report.toJson()},
                                                         {dir + "/ingest_report.txt", report.toText()}};
    for (const auto& file : files) {
        std::ofstream out(file.first, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + file.first;
            return false;
        }
        out.write(file.second.data(), static_cast<std::streamsize>(file.second.size()));
        if (!out.good()) {
            error = "failed while writing " + file.first;
            return false;
        }
    }
    return true;
}

} // namespace neo
