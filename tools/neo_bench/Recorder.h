#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {
namespace bench {

// One measured (or deliberately not measured) figure. Every row says which dataset
// it ran on and where that dataset came from, so a CSV can never be mistaken for
// real-data numbers when it is synthetic.
struct BenchRow {
    std::string experiment;   // lookup | range | topk | query | index_build | index_memory | master_memory | ingest ...
    std::string dataset;      // "real", "synthetic-100000", ...
    std::string datasetKind;  // real | synthetic-resampled | synthetic-parametric
    std::string variant;      // "linear scan", "dsa::HashMap", ...
    std::string param;        // "sel=0.01", "k=100", "page=1000", ""
    std::size_t n = 0;        // the dataset size the figure is about (objects or approaches, see note)
    std::size_t opsPerRepeat = 1;
    int         repeats = 0;
    double      median = 0.0;
    double      min = 0.0;
    double      max = 0.0;
    std::string unit;         // "ns/op", "ms", "bytes"
    std::uint64_t result = 0; // checksum of the workload's answer
    bool        hasResult = false;
    std::string status = "ok"; // ok | not_run
    std::string note;
};

// Collects rows and correctness failures from every experiment. A failure means two
// variants that must give the same answer did not: the run is then not trustworthy
// and neo_bench exits non-zero.
class Recorder {
public:
    void add(BenchRow row) { rows_.push_back(std::move(row)); }
    void fail(std::string message) { failures_.push_back(std::move(message)); }
    // A figure that was supposed to exist and does not, with the reason.
    void notRun(const std::string& experiment, const std::string& dataset, const std::string& datasetKind,
                const std::string& reason);

    const std::vector<BenchRow>&     rows() const { return rows_; }
    const std::vector<std::string>&  failures() const { return failures_; }

    // CSV with a header line; fields containing commas, quotes or newlines are quoted.
    bool writeCsv(const std::string& path, std::string& error) const;
    std::string csv() const;

private:
    std::vector<BenchRow>    rows_;
    std::vector<std::string> failures_;
};

std::string csvField(const std::string& text);

} // namespace bench
} // namespace neo
