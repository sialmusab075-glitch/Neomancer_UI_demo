#include "neo/model/Dataset.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

namespace neo {

std::string JoinReport::toString() const {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "join: %zu rows, %zu matched, %zu unmatched (%zu distinct designations), "
                  "%zu objects with approaches",
                  rowsSeen, rowsMatched, rowsUnmatched, distinctUnmatched, objectsWithApproaches);
    std::string out = buf;
    for (const auto& s : unmatchedSamples) {
        std::snprintf(buf, sizeof buf, "\n  unmatched: %-16s %zu row(s)", s.first.c_str(), s.second);
        out += buf;
    }
    if (distinctUnmatched > unmatchedSamples.size()) {
        std::snprintf(buf, sizeof buf, "\n  ... and %zu more unmatched designation(s)",
                      distinctUnmatched - unmatchedSamples.size());
        out += buf;
    }
    return out;
}

void Dataset::setObjects(std::vector<Asteroid>&& objects) {
    records_.clear();
    approaches_.clear();
    byDesignation_.clear();
    bySpkId_.clear();
    duplicatesDropped_ = 0;

    records_.reserve(objects.size());
    byDesignation_.reserve(objects.size() * 2);
    bySpkId_.reserve(objects.size() * 2);

    for (Asteroid& a : objects) {
        if (a.pdes.empty() || byDesignation_.count(a.pdes) != 0) {
            ++duplicatesDropped_;
            continue;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(records_.size());
        byDesignation_.emplace(a.pdes, index);
        if (!a.spkid.empty()) {
            bySpkId_.emplace(a.spkid, index);
        }
        AsteroidRecord record;
        record.object = std::move(a);
        records_.push_back(std::move(record));
    }
    objects.clear();
}

JoinReport Dataset::joinApproaches(std::vector<ParsedApproach>&& rows) {
    JoinReport report;
    report.rowsSeen = rows.size();

    // Resolve designations first, so the sort below works on indices only.
    // std::map keeps the unmatched sample in designation order, which makes the
    // validation report reproducible between runs.
    std::map<std::string, std::size_t> unmatched;
    std::vector<CloseApproach> matched;
    matched.reserve(rows.size());
    for (ParsedApproach& row : rows) {
        const std::uint32_t index = find(row.designation);
        if (index == kInvalidRecord) {
            ++unmatched[row.designation];
            continue;
        }
        row.approach.objectIndex = index;
        matched.push_back(row.approach);
    }
    rows.clear();

    report.rowsMatched = matched.size();
    report.rowsUnmatched = report.rowsSeen - report.rowsMatched;
    report.distinctUnmatched = unmatched.size();
    for (const auto& entry : unmatched) {
        if (report.unmatchedSamples.size() >= JoinReport::kMaxUnmatchedSamples) {
            break;
        }
        report.unmatchedSamples.emplace_back(entry.first, entry.second);
    }

    approaches_ = std::move(matched);
    rebuildRanges(report.objectsWithApproaches);
    return report;
}

std::size_t Dataset::setApproaches(std::vector<CloseApproach>&& approaches) {
    std::size_t dropped = 0;
    approaches_.clear();
    approaches_.reserve(approaches.size());
    for (CloseApproach& a : approaches) {
        if (a.objectIndex >= records_.size()) {
            ++dropped; // a foreign key that points nowhere: never stored
            continue;
        }
        approaches_.push_back(a);
    }
    approaches.clear();
    std::size_t objectsWithApproaches = 0;
    rebuildRanges(objectsWithApproaches);
    return dropped;
}

void Dataset::rebuildRanges(std::size_t& objectsWithApproaches) {
    // Group by object, then by date inside each object. O(n log n) once.
    std::sort(approaches_.begin(), approaches_.end(), [](const CloseApproach& a, const CloseApproach& b) {
        if (a.objectIndex != b.objectIndex) {
            return a.objectIndex < b.objectIndex;
        }
        return a.jdTdb < b.jdTdb;
    });
    for (AsteroidRecord& r : records_) {
        r.firstApproach = 0;
        r.approachCount = 0;
    }
    std::size_t i = 0;
    while (i < approaches_.size()) {
        const std::uint32_t owner = approaches_[i].objectIndex;
        std::size_t j = i;
        while (j < approaches_.size() && approaches_[j].objectIndex == owner) {
            ++j;
        }
        AsteroidRecord& record = records_[owner];
        record.firstApproach = static_cast<std::uint32_t>(i);
        record.approachCount = static_cast<std::uint32_t>(j - i);
        ++objectsWithApproaches;
        i = j;
    }
}

std::uint32_t Dataset::find(const std::string& pdes) const {
    const auto it = byDesignation_.find(pdes);
    return it == byDesignation_.end() ? kInvalidRecord : it->second;
}

std::uint32_t Dataset::findBySpkId(const std::string& spkid) const {
    const auto it = bySpkId_.find(spkid);
    return it == bySpkId_.end() ? kInvalidRecord : it->second;
}

ApproachSpan Dataset::approachesOf(std::uint32_t recordIndex) const {
    ApproachSpan span;
    if (recordIndex >= records_.size()) {
        return span;
    }
    const AsteroidRecord& r = records_[recordIndex];
    if (r.approachCount == 0) {
        return span;
    }
    span.data = approaches_.data() + r.firstApproach;
    span.count = r.approachCount;
    return span;
}

void Dataset::clear() {
    records_.clear();
    approaches_.clear();
    byDesignation_.clear();
    bySpkId_.clear();
    duplicatesDropped_ = 0;
}

} // namespace neo
