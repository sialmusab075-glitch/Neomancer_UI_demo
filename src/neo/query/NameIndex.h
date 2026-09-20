#pragma once

#include "neo/model/Dataset.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {

// Case-insensitive PREFIX search over names and designations.
//
// WHAT IT IS FOR
//   The search box: typing "ero" finds 433 Eros, "2020 ab" finds every 2020 AB*,
//   "433" finds the numbered asteroid. Exact designation lookup is the HashMap's
//   job; this covers "starts with", which a hash cannot answer.
//
// HOW
//   A sorted array of (lower-cased key, record index). Every object contributes
//   its designation, and its name too when it has one, so a search matches
//   either. All the keys that share a prefix are contiguous in sorted order, so
//   a prefix is a half-open range [lowerBound(prefix), lowerBound(next prefix)):
//   two O(log n) binary searches, then a straight read of the matches.
//
// COMPLEXITY (n keys, m matches)
//   build     O(n log n), by the stable merge sort in dsa/Sort.h
//   search    O(log n + m)
//   count     O(log n): the match count is EXACT without reading the matches,
//             which is what lets the planner use this index with no histogram
//   space     n * (key + 4 bytes); ~2 keys per named object, 1 per unnamed one
//
// "Case-insensitive" means ASCII case folding, which is all designations and IAU
// names need; bytes >= 0x80 (accented names) compare as they are.

std::string toLowerAscii(const std::string& text);
bool startsWithIgnoreCase(const std::string& text, const std::string& lowerPrefix);

class NameIndex {
public:
    void build(const Dataset& dataset);
    void clear();

    // Number of index ENTRIES with this prefix (an object matched by both its
    // name and its designation counts twice). An upper bound on the distinct
    // records; exact when no object matches on both.
    std::size_t countPrefix(const std::string& lowerPrefix) const;

    // Distinct record indices whose name or designation starts with the prefix,
    // ascending. `lowerPrefix` must already be lower-case.
    std::vector<std::uint32_t> matches(const std::string& lowerPrefix) const;

    std::size_t size() const { return entries_.size(); }
    std::size_t memoryBytes() const;
    bool checkInvariants(std::string& error) const;

private:
    struct Entry {
        std::string   key;
        std::uint32_t record = 0;
    };

    std::pair<std::size_t, std::size_t> prefixRange(const std::string& lowerPrefix) const;

    std::vector<Entry> entries_;
};

} // namespace neo
