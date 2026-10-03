// neo_bench: the stage 7 benchmarks of docs/NEO_PLAN.md section 8, written to CSV plus a
// markdown summary.
//
//   neo_bench                         the full run (needs data/neo.db for the real rows)
//   neo_bench --quick                 under a second, small sizes, for a smoke check
//   neo_bench --only lookup,topk      a subset of the experiments
//
// It reads the database only: no network, no SQL. Without a real database the real-data
// rows are recorded as NOT RUN with the reason, and the sizes are generated: they are
// labelled synthetic in the CSV and in the summary, and are never to be quoted as real.

#include "Experiments.h"
#include "Summary.h"
#include "SyntheticData.h"
#include "Timing.h"
#include "neo/model/JulianDate.h"
#include "neo/storage/Database.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using neo::bench::Clock;

void printUsage() {
    std::printf(
        "neo_bench - benchmarks for the NEO data structures and query engine\n"
        "\n"
        "Usage: neo_bench [options]\n"
        "  --db PATH               real database (default: data/neo.db)\n"
        "  --out-dir DIR           where neo_bench.csv and neo_bench_summary.md go (default: bench-results)\n"
        "  --sizes A,B             sizes cut from the real data (default 1000,10000); generated when there is no real data\n"
        "  --synthetic A,B         generated sizes (default 100000,500000); resampled from the real data when present\n"
        "  --repeats N             timed repeats per figure, at least 5 (default 7)\n"
        "  --seed N                seed of every generator (default 42)\n"
        "  --only LIST             comma list of: lookup, range, mutation, topk, query, memory, ingest (default all)\n"
        "  --ingest-objects N      objects in the ingestion-by-page-size experiment (default 20000)\n"
        "  --page-sizes A,B,C      page sizes of that experiment (default 100,500,1000,2500,5000)\n"
        "  --no-real               ignore the real database even if it exists\n"
        "  --quick                 sizes 1000, synthetic 5000, 5 repeats, ingest 2000 objects\n"
        "  -h, --help\n");
}

std::vector<std::size_t> parseList(const std::string& text, bool& ok) {
    std::vector<std::size_t> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(item.c_str(), &end, 10);
        if (item.empty() || end == item.c_str() || *end != '\0' || v == 0) {
            ok = false;
            return {};
        }
        values.push_back(static_cast<std::size_t>(v));
    }
    return values;
}

struct Options {
    std::string dbPath = "data/neo.db";
    std::string outDir = "bench-results";
    std::vector<std::size_t> sizes = {1000, 10000};
    std::vector<std::size_t> synthetic = {100000, 500000};
    std::vector<std::size_t> pageSizes = {100, 500, 1000, 2500, 5000};
    std::size_t ingestObjects = 20000;
    int repeats = 7;
    std::uint64_t seed = 42;
    std::set<std::string> only;
    bool noReal = false;
};

bool wants(const Options& o, const char* name) { return o.only.empty() || o.only.count(name) != 0; }

double secondsSince(const Clock::time_point& t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

struct Prepared {
    std::string name;
    std::string kind;
    std::unique_ptr<neo::Dataset> dataset;
};

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::printf("error: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        bool listOk = true;
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "--db") {
            o.dbPath = value("--db");
        } else if (arg == "--out-dir") {
            o.outDir = value("--out-dir");
        } else if (arg == "--sizes") {
            o.sizes = parseList(value("--sizes"), listOk);
        } else if (arg == "--synthetic") {
            o.synthetic = parseList(value("--synthetic"), listOk);
        } else if (arg == "--page-sizes") {
            o.pageSizes = parseList(value("--page-sizes"), listOk);
        } else if (arg == "--ingest-objects") {
            const std::vector<std::size_t> one = parseList(value("--ingest-objects"), listOk);
            if (listOk && one.size() == 1) {
                o.ingestObjects = one[0];
            } else {
                listOk = false;
            }
        } else if (arg == "--repeats") {
            o.repeats = std::atoi(value("--repeats"));
        } else if (arg == "--seed") {
            o.seed = std::strtoull(value("--seed"), nullptr, 10);
        } else if (arg == "--only") {
            std::stringstream stream(value("--only"));
            std::string item;
            while (std::getline(stream, item, ',')) {
                o.only.insert(item);
            }
        } else if (arg == "--no-real") {
            o.noReal = true;
        } else if (arg == "--quick") {
            o.sizes = {1000};
            o.synthetic = {5000};
            o.repeats = 5;
            o.ingestObjects = 2000;
            o.pageSizes = {100, 500, 1000};
        } else {
            std::printf("error: unknown option %s\n", arg.c_str());
            printUsage();
            return 2;
        }
        if (!listOk) {
            std::printf("error: %s expects comma-separated positive integers\n", arg.c_str());
            return 2;
        }
    }
    if (o.repeats < 5) {
        std::printf("note: --repeats raised to 5 (the plan's minimum)\n");
        o.repeats = 5;
    }

    neo::bench::ExperimentConfig config;
    config.repeats = o.repeats;
    config.seed = o.seed;
    neo::bench::Recorder recorder;
    neo::bench::RunInfo info;
    info.compiler = neo::bench::compilerString();
    info.buildType = neo::bench::buildTypeString();
    info.platform = neo::bench::platformString();
    info.startedUtc = neo::utcNowIso();
    info.threads = std::thread::hardware_concurrency();
    info.seed = o.seed;
    info.repeats = neo::bench::effectiveRepeats(config);

    std::printf("neo_bench: %s, %s, %s, %d repeats\n", info.platform.c_str(), info.compiler.c_str(), info.buildType.c_str(),
                info.repeats);
#if !defined(NDEBUG)
    std::printf("WARNING: this is not an optimised build; the figures are not representative.\n");
#endif

    // --- the real dataset --------------------------------------------------------------
    std::unique_ptr<neo::Dataset> real;
    std::string realReason;
    if (o.noReal) {
        realReason = "--no-real";
    } else if (!std::filesystem::exists(o.dbPath)) {
        realReason = o.dbPath + " not found (run neo_ingest, or copy neo.db here)";
    } else {
        real = std::make_unique<neo::Dataset>();
        neo::DatabaseMeta meta;
        const Clock::time_point t = Clock::now();
        const neo::DbStatus status = neo::loadDatabase(o.dbPath, *real, meta);
        if (!status) {
            realReason = "could not load " + o.dbPath + ": " + status.error;
            real.reset();
        } else {
            std::printf("real dataset: %zu objects, %zu approaches, loaded in %.2f s\n", real->objectCount(),
                        real->approachCount(), secondsSince(t));
        }
    }
    info.dbPath = real ? o.dbPath : "(none: " + realReason + ")";
    if (!real) {
        std::printf("real dataset: NOT RUN: %s\n", realReason.c_str());
        recorder.notRun("all experiments", "real", "real", realReason);
        neo::bench::DatasetSummary missing;
        missing.name = "real (all NEOs)";
        missing.kind = "real";
        missing.status = "not_run: " + realReason;
        info.datasets.push_back(missing);
    }

    // --- the datasets to run on ----------------------------------------------------------
    std::vector<Prepared> prepared;
    auto addSynthetic = [&](std::size_t n, const neo::Dataset* resampleFrom, const char* prefix) {
        const std::string name = std::string(prefix) + "-" + std::to_string(n);
        for (const Prepared& existing : prepared) {
            if (existing.name == name) {
                return;
            }
        }
        neo::bench::SyntheticOptions options;
        options.objects = n;
        options.seed = o.seed + n;
        options.resampleFrom = resampleFrom;
        neo::bench::SyntheticInfo made;
        const Clock::time_point t = Clock::now();
        Prepared p;
        p.dataset = std::make_unique<neo::Dataset>(neo::bench::makeSyntheticDataset(options, &made));
        p.name = name;
        p.kind = made.kind;
        std::printf("generated %s (%s): %zu objects, %zu approaches in %.2f s\n", p.name.c_str(), p.kind.c_str(),
                    made.objects, made.approaches, secondsSince(t));
        prepared.push_back(std::move(p));
    };
    for (const std::size_t n : o.sizes) {
        if (real && n < real->objectCount()) {
            Prepared p;
            p.dataset = std::make_unique<neo::Dataset>(neo::bench::prefixOfDataset(*real, n));
            p.name = "real-first-" + std::to_string(n);
            p.kind = "real";
            prepared.push_back(std::move(p));
        } else if (!real) {
            addSynthetic(n, nullptr, "synthetic");
        }
    }
    if (real) {
        Prepared p;
        p.name = "real";
        p.kind = "real";
        p.dataset = std::move(real);
        prepared.push_back(std::move(p));
    }
    const neo::Dataset* resampleFrom = nullptr;
    for (const Prepared& p : prepared) {
        if (p.name == "real") {
            resampleFrom = p.dataset.get();
        }
    }
    for (const std::size_t n : o.synthetic) {
        addSynthetic(n, resampleFrom, "synthetic");
    }

    // --- run ---------------------------------------------------------------------------------
    for (Prepared& p : prepared) {
        std::printf("\n== %s (%s): %zu objects, %zu approaches\n", p.name.c_str(), p.kind.c_str(), p.dataset->objectCount(),
                    p.dataset->approachCount());
        neo::bench::DatasetSummary summary;
        summary.name = p.name;
        summary.kind = p.kind;
        summary.objects = p.dataset->objectCount();
        summary.approaches = p.dataset->approachCount();
        info.datasets.push_back(summary);

        neo::QueryEngine engine(*p.dataset);
        const Clock::time_point t = Clock::now();
        engine.build();
        std::printf("   indexes built in %.2f s\n", secondsSince(t));

        neo::bench::DatasetUnderTest data;
        data.name = p.name;
        data.kind = p.kind;
        data.dataset = p.dataset.get();
        data.engine = &engine;
        auto run = [&](const char* name, void (*fn)(const neo::bench::DatasetUnderTest&, const neo::bench::ExperimentConfig&,
                                                    neo::bench::Recorder&)) {
            if (!wants(o, name)) {
                return;
            }
            const Clock::time_point start = Clock::now();
            std::printf("   %-7s ... ", name);
            std::fflush(stdout);
            fn(data, config, recorder);
            std::printf("%.1f s\n", secondsSince(start));
        };
        run("lookup", neo::bench::runLookupExperiment);
        run("range", neo::bench::runRangeExperiment);
        run("mutation", neo::bench::runMutationExperiment);
        run("topk", neo::bench::runTopKExperiment);
        run("query", neo::bench::runQueryExperiment);
        run("memory", neo::bench::runMemoryExperiments);
    }
    if (wants(o, "ingest")) {
        std::printf("\n== ingestion by page size (%zu objects)\n", o.ingestObjects);
        const Clock::time_point t = Clock::now();
        neo::bench::runIngestExperiment(config, o.ingestObjects, o.pageSizes, recorder);
        std::printf("   %.1f s\n", secondsSince(t));
    }

    // --- output --------------------------------------------------------------------------------
    std::error_code ec;
    std::filesystem::create_directories(o.outDir, ec);
    const std::string csvPath = (std::filesystem::path(o.outDir) / "neo_bench.csv").string();
    const std::string mdPath = (std::filesystem::path(o.outDir) / "neo_bench_summary.md").string();
    std::string error;
    if (!recorder.writeCsv(csvPath, error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    {
        std::ofstream md(mdPath, std::ios::binary | std::ios::trunc);
        md << neo::bench::summaryMarkdown(recorder, info);
        if (!md) {
            std::printf("error: cannot write %s\n", mdPath.c_str());
            return 1;
        }
    }
    std::printf("\nwrote %s (%zu rows) and %s\n", csvPath.c_str(), recorder.rows().size(), mdPath.c_str());
    if (!recorder.failures().empty()) {
        std::printf("\nCORRECTNESS FAILURES (%zu): the timings are not trustworthy\n", recorder.failures().size());
        for (const std::string& f : recorder.failures()) {
            std::printf("  - %s\n", f.c_str());
        }
        return 1;
    }
    return 0;
}
