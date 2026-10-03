#pragma once

#include "sat/model/ElementSet.h"

#include <string>
#include <vector>

namespace sat {

constexpr int kCatalogSchemaVersion = 1;

struct CatalogMeta {
    int         schemaVersion = kCatalogSchemaVersion;
    std::string createdUtc;
    std::string source;                // "CelesTrak GP (OMM JSON)"
    std::vector<std::string> groups;   // what was requested: "stations", "visual", ...
};

// The element sets the app draws, as one JSON file (a few hundred sets, so no database): a
// document with the metadata and an array of OMM-style records. Epochs are stored both as ISO text
// and as the exact (whole, fraction) pair, so a save/load round trip is bit-exact.
//
// Written atomically: to "<path>.tmp", flushed, then renamed over the old file, so a crash leaves
// the previous catalogue intact.
bool saveCatalog(const std::string& path, const std::vector<ElementSet>& sets, const CatalogMeta& meta, std::string& error);
bool loadCatalog(const std::string& path, std::vector<ElementSet>& sets, CatalogMeta& meta, std::string& error);

// The same OMM-style record the catalogue stores (used by the tests to build ingest responses).
std::string toOmmJson(const std::vector<ElementSet>& sets);

} // namespace sat
