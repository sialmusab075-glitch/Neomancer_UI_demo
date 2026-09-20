#include "neo/ingest/ParseReport.h"

#include <cstdio>
#include <utility>

namespace neo {

void ValidationReport::noteRejected(std::size_t index, std::string reason) {
    ++rowsRejected;
    if (rejectedSamples.size() < kMaxRejectedSamples) {
        rejectedSamples.push_back(RejectedRow{index, std::move(reason)});
    }
}

std::size_t ValidationReport::nullCount(const std::string& field) const {
    for (const auto& entry : nullCounts) {
        if (entry.first == field) {
            return entry.second;
        }
    }
    return 0;
}

std::string ValidationReport::toString() const {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s (signature version %s)\n  rows: %zu seen, %zu accepted, %zu rejected",
                  signatureSource.c_str(), signatureVersion.c_str(), rowsSeen, rowsAccepted, rowsRejected);
    std::string out = buf;
    if (derivedDistanceRanges > 0) {
        std::snprintf(buf, sizeof buf, "\n  derived dist_min/dist_max from dist: %zu row(s)", derivedDistanceRanges);
        out += buf;
    }
    for (const auto& entry : nullCounts) {
        if (entry.second == 0) {
            continue;
        }
        const double pct = rowsSeen > 0 ? 100.0 * static_cast<double>(entry.second) / static_cast<double>(rowsSeen)
                                        : 0.0;
        std::snprintf(buf, sizeof buf, "\n  null %-16s %7zu  (%5.1f%%)", entry.first.c_str(), entry.second, pct);
        out += buf;
    }
    for (const RejectedRow& r : rejectedSamples) {
        std::snprintf(buf, sizeof buf, "\n  rejected row %zu: %s", r.index, r.reason.c_str());
        out += buf;
    }
    if (rowsRejected > rejectedSamples.size()) {
        std::snprintf(buf, sizeof buf, "\n  ... and %zu more rejected row(s)", rowsRejected - rejectedSamples.size());
        out += buf;
    }
    return out;
}

void ValidationReport::merge(const ValidationReport& other) {
    if (signatureVersion.empty()) {
        signatureVersion = other.signatureVersion;
        signatureSource = other.signatureSource;
    }
    rowsSeen += other.rowsSeen;
    rowsAccepted += other.rowsAccepted;
    rowsRejected += other.rowsRejected;
    derivedDistanceRanges += other.derivedDistanceRanges;
    for (const auto& entry : other.nullCounts) {
        bool found = false;
        for (auto& mine : nullCounts) {
            if (mine.first == entry.first) {
                mine.second += entry.second;
                found = true;
                break;
            }
        }
        if (!found) {
            nullCounts.push_back(entry);
        }
    }
    for (const RejectedRow& r : other.rejectedSamples) {
        if (rejectedSamples.size() >= kMaxRejectedSamples) {
            break;
        }
        rejectedSamples.push_back(r);
    }
}

} // namespace neo
