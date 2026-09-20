#include "neo/query/NameIndex.h"

#include "neo/dsa/Sort.h"

#include <algorithm>

namespace neo {

std::string toLowerAscii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool startsWithIgnoreCase(const std::string& text, const std::string& lowerPrefix) {
    if (text.size() < lowerPrefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lowerPrefix.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != lowerPrefix[i]) {
            return false;
        }
    }
    return true;
}

void NameIndex::build(const Dataset& dataset) {
    clear();
    std::vector<Entry> raw;
    raw.reserve(dataset.records().size() + dataset.records().size() / 8);
    for (std::uint32_t i = 0; i < dataset.records().size(); ++i) {
        const Asteroid& a = dataset.records()[i].object;
        Entry byDesignation;
        byDesignation.key = toLowerAscii(a.pdes);
        byDesignation.record = i;
        raw.push_back(std::move(byDesignation));
        if (!a.name.empty()) {
            Entry byName;
            byName.key = toLowerAscii(a.name);
            byName.record = i;
            raw.push_back(std::move(byName));
        }
    }

    // Sort a permutation of the entries with the stable merge sort, then lay the
    // entries out in that order. Ties on the key fall back to the record index.
    std::vector<std::uint32_t> order(raw.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    dsa::mergeSort(order, [&raw](std::uint32_t a, std::uint32_t b) {
        const int c = raw[a].key.compare(raw[b].key);
        if (c != 0) {
            return c < 0;
        }
        return raw[a].record < raw[b].record;
    });
    entries_.reserve(raw.size());
    for (const std::uint32_t position : order) {
        entries_.push_back(std::move(raw[position]));
    }
}

void NameIndex::clear() { entries_.clear(); }

std::pair<std::size_t, std::size_t> NameIndex::prefixRange(const std::string& lowerPrefix) const {
    const std::size_t n = entries_.size();
    if (n == 0) {
        return {0, 0};
    }
    // First entry whose key is >= the prefix.
    const std::size_t begin =
        dsa::partitionPoint(n, [this, &lowerPrefix](std::size_t i) { return entries_[i].key < lowerPrefix; });

    // Every key with this prefix sorts before the prefix with its last byte
    // incremented, so that is the exclusive upper end. (A prefix ending in 0xFF
    // carries into the byte before it; an all-0xFF prefix runs to the end.)
    std::string upper = lowerPrefix;
    while (!upper.empty() && static_cast<unsigned char>(upper.back()) == 0xFF) {
        upper.pop_back();
    }
    std::size_t end = n;
    if (!upper.empty()) {
        upper.back() = static_cast<char>(static_cast<unsigned char>(upper.back()) + 1);
        end = dsa::partitionPoint(n, [this, &upper](std::size_t i) { return entries_[i].key < upper; });
    }
    return {begin, std::max(begin, end)};
}

std::size_t NameIndex::countPrefix(const std::string& lowerPrefix) const {
    const std::pair<std::size_t, std::size_t> range = prefixRange(lowerPrefix);
    return range.second - range.first;
}

std::vector<std::uint32_t> NameIndex::matches(const std::string& lowerPrefix) const {
    const std::pair<std::size_t, std::size_t> range = prefixRange(lowerPrefix);
    std::vector<std::uint32_t> out;
    out.reserve(range.second - range.first);
    for (std::size_t i = range.first; i < range.second; ++i) {
        out.push_back(entries_[i].record);
    }
    // An object can match through both its name and its designation.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::size_t NameIndex::memoryBytes() const {
    std::size_t bytes = entries_.capacity() * sizeof(Entry);
    for (const Entry& e : entries_) {
        // Keys beyond the small-string buffer own heap memory.
        if (e.key.capacity() > sizeof(std::string)) {
            bytes += e.key.capacity();
        }
    }
    return bytes;
}

bool NameIndex::checkInvariants(std::string& error) const {
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (entries_[i].key < entries_[i - 1].key) {
            error = "name index entry " + std::to_string(i) + " sorts before its predecessor";
            return false;
        }
    }
    return true;
}

} // namespace neo
