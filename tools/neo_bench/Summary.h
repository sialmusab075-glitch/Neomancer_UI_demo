#pragma once

#include "Recorder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {
namespace bench {

struct DatasetSummary {
    std::string name;
    std::string kind;
    std::size_t objects = 0;
    std::size_t approaches = 0;
    std::string status = "ok"; // ok, or "not_run: <reason>"
};

struct RunInfo {
    std::string compiler;
    std::string buildType;
    std::string platform;
    std::string startedUtc;
    unsigned    threads = 0;
    std::uint64_t seed = 0;
    int         repeats = 0;
    std::string dbPath;
    std::vector<DatasetSummary> datasets;
};

std::string compilerString();
std::string buildTypeString();
std::string platformString();

// The report that goes next to the CSV: environment, datasets (with their kind and
// whether they ran), one table per experiment and dataset with each variant's median,
// its spread and its ratio to the first variant of the group, then every correctness
// failure. Written so that it cannot be read as real-data numbers when it is not.
std::string summaryMarkdown(const Recorder& recorder, const RunInfo& info);

} // namespace bench
} // namespace neo
