#pragma once

#include "neo/model/Dataset.h"

#include <cstddef>
#include <functional>
#include <string>

namespace neo {

// neo.db is STORAGE ONLY. Queries run on the in-memory DSA structures built
// from this data (stage 5/6); nothing in the query path issues SQL. SQLite may
// reappear in neo_bench as a baseline to compare against, and nowhere else.
// That is why this schema carries only the indexes integrity needs (the primary
// keys and the UNIQUE designations) and no query indexes: a "helpful" index on
// approaches(jd) would be dead weight that also flatters the SQL baseline.

// Bumped whenever the schema changes. A file written by a different version is
// refused with a message telling the user to re-run neo_ingest.
constexpr int kSchemaVersion = 1;

struct DatabaseMeta {
    int         schemaVersion = kSchemaVersion;
    std::string createdUtc;
    std::string sbdbApiVersion;
    std::string cadApiVersion;
    std::string cadDateMin;
    std::string cadDateMax;
    double      cadDistMaxAU = 0.0;
    std::size_t objectCount = 0;
    std::size_t approachCount = 0;
    // Fingerprint of the ingest report that produced this file, so a database
    // can be traced back to the run (and its validation numbers) that made it.
    std::string reportChecksum;
};

struct DbStatus {
    bool        ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
    static DbStatus success() { return DbStatus{true, std::string()}; }
    static DbStatus failure(std::string message) { return DbStatus{false, std::move(message)}; }
};

// Test hooks for the interrupted-write case. Each returns false to abort the
// save exactly there, as if the process had died at that point.
struct SaveHooks {
    std::function<bool()> beforeCommit;  // rows written, transaction still open
    std::function<bool()> beforeReplace; // temp file complete and verified
};

// Writes `dataset` to `path` atomically:
//   1. build <path>.tmp in one transaction with prepared statements
//   2. verify the row counts and the foreign keys inside the temp file
//   3. move the existing file aside, rename the temp into place, delete the old
// A failure at any step leaves the previous `path` intact.
DbStatus saveDatabase(const Dataset& dataset, const DatabaseMeta& meta, const std::string& path,
                      const SaveHooks& hooks = SaveHooks());

// Loads a database into the in-memory model: master record vector plus the flat
// approach vector with per-object ranges. Refuses a schema version this build
// does not know.
DbStatus loadDatabase(const std::string& path, Dataset& dataset, DatabaseMeta& meta);

// Reads only the meta row (row counts, provenance, schema version) without
// touching the data tables: what the UI status strip needs at startup.
DbStatus readDatabaseMeta(const std::string& path, DatabaseMeta& meta);

// Size of a database file in bytes, or 0 when it cannot be read.
std::size_t databaseFileSize(const std::string& path);

} // namespace neo
