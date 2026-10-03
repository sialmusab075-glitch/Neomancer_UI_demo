// neo_bench harness tests: the timing helpers, the synthetic generator, the SBDB page
// writer, and every experiment run on small datasets. The experiments check themselves
// (every variant of an experiment must return the same answer), so a clean run here
// means the benchmark compares equal work. No network, no timing thresholds: this
// suite must pass on a slow, noisy machine, so it tests what is computed, not how
// fast. Run from the project root so the optional real-dataset pass finds data/neo.db.

#include "Experiments.h"
#include "Recorder.h"
#include "Summary.h"
#include "SyntheticData.h"
#include "Timing.h"
#include "neo/ingest/ParseReport.h"
#include "neo/ingest/SbdbParser.h"
#include "neo/query/QueryEngine.h"
#include "neo/storage/Database.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

bool near(double a, double b, double rel = 1e-12) {
    return std::fabs(a - b) <= rel * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

// FNV over the fields a benchmark depends on, to compare two datasets exactly.
std::uint64_t fingerprint(const neo::Dataset& ds) {
    std::uint64_t acc = neo::bench::kChecksumSeed;
    for (const neo::AsteroidRecord& r : ds.records()) {
        for (const char c : r.object.pdes) {
            acc = neo::bench::mixChecksum(acc, static_cast<unsigned char>(c));
        }
        acc = neo::bench::mixChecksum(acc, static_cast<std::uint64_t>(r.object.orbital.semiMajorAxisAU * 1e9));
        acc = neo::bench::mixChecksum(acc, r.object.physical.absoluteMagnitudeH ? 1u : 0u);
        acc = neo::bench::mixChecksum(acc, r.approachCount);
    }
    for (const neo::CloseApproach& c : ds.approaches()) {
        acc = neo::bench::mixChecksum(acc, static_cast<std::uint64_t>(c.jdTdb * 1e3));
        acc = neo::bench::mixChecksum(acc, static_cast<std::uint64_t>(c.distanceAU * 1e12));
    }
    return acc;
}

// A minimal quote-aware CSV line splitter (the fields we write may contain commas).
std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    fields.push_back(cur);
    return fields;
}

// --- timing helpers -----------------------------------------------------------------

void testTiming() {
    std::printf("[timing] median, repeats, warm-up, stability\n");
    check(neo::bench::medianOf({}) == 0.0, "the median of nothing is 0");
    check(neo::bench::medianOf({5.0}) == 5.0, "one sample");
    check(neo::bench::medianOf({3.0, 1.0, 2.0}) == 2.0, "odd count: the middle one, whatever the order");
    check(neo::bench::medianOf({4.0, 1.0, 3.0, 2.0}) == 2.5, "even count: the mean of the middle two");
    check(neo::bench::medianOf({1.0, 1.0, 1000.0}) == 1.0, "an outlier does not move the median");

    int calls = 0;
    neo::bench::Timing t = neo::bench::measureNsPerOp(7, 10, [&] {
        ++calls;
        return std::uint64_t(99);
    });
    check(calls == 8, "one warm-up plus 7 timed repeats", std::to_string(calls));
    check(t.repeats() == 7 && t.stable && t.result == 99, "7 samples, stable, result kept");
    check(t.min() <= t.median() && t.median() <= t.max() && t.min() >= 0.0, "min <= median <= max, all non-negative");

    std::uint64_t counter = 0;
    const neo::bench::Timing unstable = neo::bench::measureNsPerOp(5, 1, [&] { return ++counter; });
    check(!unstable.stable, "a workload that returns different results is flagged unstable");

    const neo::bench::Timing ms = neo::bench::measureMs(5, [] {
        volatile std::uint64_t sink = 0;
        for (int i = 0; i < 1000; ++i) {
            sink = sink + static_cast<std::uint64_t>(i);
        }
        return std::uint64_t(sink);
    });
    check(ms.repeats() == 5 && ms.max() < 1000.0, "measureMs gives milliseconds (a tiny loop is far below a second)");
}

void testCsv() {
    std::printf("[csv] quoting, header, not-run rows\n");
    check(neo::bench::csvField("plain") == "plain", "a plain field is untouched");
    check(neo::bench::csvField("a,b") == "\"a,b\"", "a comma forces quotes");
    check(neo::bench::csvField("say \"hi\"") == "\"say \"\"hi\"\"\"", "quotes are doubled");
    check(neo::bench::csvField("two\nlines") == "\"two\nlines\"", "a newline forces quotes");

    neo::bench::Recorder rec;
    neo::bench::BenchRow row;
    row.experiment = "lookup";
    row.dataset = "d";
    row.datasetKind = "synthetic-parametric";
    row.variant = "v, with comma";
    row.n = 10;
    row.repeats = 5;
    row.median = 2.0;
    row.min = 1.0;
    row.max = 3.0;
    row.unit = "ns/op";
    rec.add(row);
    rec.notRun("all experiments", "real", "real", "neo.db not found");
    const std::string csv = rec.csv();
    std::istringstream lines(csv);
    std::string line;
    std::getline(lines, line);
    const std::size_t columns = splitCsvLine(line).size();
    check(columns == 15 && line.rfind("experiment,dataset,dataset_kind", 0) == 0, "the header has 15 columns", line);
    bool allSame = true;
    int rows = 0;
    std::vector<std::string> last;
    while (std::getline(lines, line)) {
        last = splitCsvLine(line);
        allSame = allSame && last.size() == columns;
        ++rows;
    }
    check(rows == 2 && allSame, "every row has as many fields as the header");
    check(last.size() == 15 && last[13] == "not_run" && last[8].empty() && last[14] == "neo.db not found",
          "a not-run row has no figures, its status and its reason");
}

// --- generator --------------------------------------------------------------------------

void testDesignations() {
    std::printf("[synthetic] designations\n");
    std::set<std::string> seen;
    bool shortEnough = true;
    for (std::size_t i = 0; i < 300000; ++i) {
        const std::string d = neo::bench::syntheticDesignation(i);
        shortEnough = shortEnough && d.size() <= 15;
        seen.insert(d);
    }
    check(seen.size() == 300000, "300,000 indices give 300,000 distinct designations", std::to_string(seen.size()));
    check(shortEnough, "every designation fits the small-string buffer");
}

void testParametric() {
    std::printf("[synthetic] parametric generator\n");
    neo::bench::SyntheticOptions options;
    options.objects = 20000;
    options.seed = 7;
    neo::bench::SyntheticInfo info;
    const neo::Dataset a = neo::bench::makeSyntheticDataset(options, &info);
    check(info.kind == "synthetic-parametric", "labelled parametric", info.kind);
    check(a.objectCount() == 20000 && info.objects == 20000 && a.duplicatesDropped() == 0, "20,000 objects, none dropped");
    check(info.approaches == a.approachCount() && a.approachCount() > 10000, "approaches counted", std::to_string(a.approachCount()));

    const neo::Dataset b = neo::bench::makeSyntheticDataset(options);
    check(fingerprint(a) == fingerprint(b), "the same seed gives the same dataset");
    options.seed = 8;
    check(fingerprint(a) != fingerprint(neo::bench::makeSyntheticDataset(options)), "a different seed gives a different one");

    std::size_t withApproaches = 0, pha = 0, withH = 0, measured = 0;
    bool orbitsOk = true, approachOk = true;
    for (std::uint32_t r = 0; r < a.objectCount(); ++r) {
        const neo::Asteroid& o = a.records()[r].object;
        orbitsOk = orbitsOk && o.orbital.eccentricity > 0.0 && o.orbital.eccentricity < 1.0 && o.orbital.perihelionAU <= 1.3 &&
                   o.orbital.semiMajorAxisAU > o.orbital.perihelionAU && o.orbital.propagationSupported();
        withApproaches += a.records()[r].approachCount > 0 ? 1 : 0;
        pha += o.classification.isPHA && *o.classification.isPHA ? 1 : 0;
        withH += o.physical.absoluteMagnitudeH ? 1 : 0;
        measured += o.physical.diameterKm ? 1 : 0;
        double previous = 0.0;
        for (const neo::CloseApproach& c : a.approachesOf(r)) {
            approachOk = approachOk && c.objectIndex == r && c.jdTdb >= previous && c.distanceAU > 0.0 && c.distanceAU <= 0.05 &&
                         c.distanceMinAU <= c.distanceAU && c.distanceAU <= c.distanceMaxAU && c.relVelocityKms > 0.0;
            previous = c.jdTdb;
        }
    }
    const double n = static_cast<double>(a.objectCount());
    check(orbitsOk, "every orbit is a closed NEO ellipse (0 < e < 1, q <= 1.3 au)");
    check(approachOk, "approaches belong to their object, sorted by date, within 0.05 au, min <= nominal <= max");
    check(std::fabs(static_cast<double>(withApproaches) / n - 0.46) < 0.02, "about 46% of objects have approaches",
          std::to_string(static_cast<double>(withApproaches) / n));
    check(std::fabs(static_cast<double>(pha) / n - 0.06) < 0.01, "about 6% are PHAs");
    check(std::fabs(static_cast<double>(withH) / n - 0.995) < 0.005, "H is known for about 99.5%");
    check(std::fabs(static_cast<double>(measured) / n - 0.03) < 0.01, "about 3% have a measured diameter");
}

void testResampled() {
    std::printf("[synthetic] resampled generator, prefix cut\n");
    neo::bench::SyntheticOptions base;
    base.objects = 500;
    base.seed = 3;
    const neo::Dataset source = neo::bench::makeSyntheticDataset(base);

    neo::bench::SyntheticOptions options;
    options.objects = 3000;
    options.seed = 11;
    options.resampleFrom = &source;
    neo::bench::SyntheticInfo info;
    const neo::Dataset big = neo::bench::makeSyntheticDataset(options, &info);
    check(info.kind == "synthetic-resampled", "labelled resampled", info.kind);
    check(big.objectCount() == 3000 && big.duplicatesDropped() == 0, "3,000 objects from 500 sources");

    std::set<std::pair<double, double>> sourceOrbits;
    std::set<std::string> sourceNames;
    for (const neo::AsteroidRecord& r : source.records()) {
        sourceOrbits.insert({r.object.orbital.semiMajorAxisAU, r.object.orbital.eccentricity});
        sourceNames.insert(r.object.pdes);
    }
    bool fromSource = true, freshNames = true, datesOk = true;
    for (std::uint32_t r = 0; r < big.objectCount(); ++r) {
        const neo::Asteroid& o = big.records()[r].object;
        fromSource = fromSource && sourceOrbits.count({o.orbital.semiMajorAxisAU, o.orbital.eccentricity}) == 1;
        freshNames = freshNames && o.pdes == neo::bench::syntheticDesignation(r);
        for (const neo::CloseApproach& c : big.approachesOf(r)) {
            datesOk = datesOk && c.objectIndex == r && c.jdTdb >= 2433282.5 && c.jdTdb <= 2506331.5;
        }
    }
    check(fromSource, "every resampled object carries the real orbit of one source object");
    check(freshNames, "and a fresh designation, so lookups stay unique");
    check(datesOk, "approach dates stay inside the 1950-2150 window");
    check(fingerprint(big) == fingerprint(neo::bench::makeSyntheticDataset(options)), "resampling is deterministic");

    const neo::Dataset cut = neo::bench::prefixOfDataset(source, 100);
    std::size_t expectedApproaches = 0;
    bool same = cut.objectCount() == 100;
    for (std::uint32_t r = 0; r < 100; ++r) {
        expectedApproaches += source.records()[r].approachCount;
        same = same && cut.records()[r].object.pdes == source.records()[r].object.pdes &&
               cut.records()[r].approachCount == source.records()[r].approachCount;
    }
    check(same && cut.approachCount() == expectedApproaches, "a prefix keeps the first objects and exactly their approaches");
    check(neo::bench::prefixOfDataset(source, 100000).objectCount() == source.objectCount(), "a prefix larger than the data is the whole data");
}

void testSbdbPages() {
    std::printf("[synthetic] SBDB page writer round-trips through the real parser\n");
    neo::bench::SyntheticOptions options;
    options.objects = 700;
    options.seed = 5;
    const neo::Dataset ds = neo::bench::makeSyntheticDataset(options);

    std::vector<neo::Asteroid> parsed;
    std::size_t rejected = 0, accepted = 0;
    bool statusOk = true;
    for (std::size_t first = 0; first < ds.objectCount(); first += 250) {
        neo::ValidationReport report;
        const neo::ParseStatus status = neo::parseSbdbObjects(neo::bench::makeSbdbPageJson(ds, first, 250), parsed, report);
        statusOk = statusOk && status.ok;
        rejected += report.rowsRejected;
        accepted += report.rowsAccepted;
    }
    check(statusOk && accepted == 700 && rejected == 0 && parsed.size() == 700, "3 pages of 250/250/200: all 700 rows accepted",
          std::to_string(accepted) + " accepted, " + std::to_string(rejected) + " rejected");

    bool equal = parsed.size() == ds.objectCount();
    for (std::size_t r = 0; r < parsed.size() && equal; ++r) {
        const neo::Asteroid& a = ds.records()[r].object;
        const neo::Asteroid& p = parsed[r];
        equal = a.pdes == p.pdes && a.name == p.name && a.classification.orbitClass == p.classification.orbitClass &&
                a.classification.isPHA == p.classification.isPHA && near(a.orbital.semiMajorAxisAU, p.orbital.semiMajorAxisAU) &&
                near(a.orbital.eccentricity, p.orbital.eccentricity) && near(a.orbital.inclinationDeg, p.orbital.inclinationDeg) &&
                a.physical.absoluteMagnitudeH.has_value() == p.physical.absoluteMagnitudeH.has_value() &&
                a.orbital.moidAU.has_value() == p.orbital.moidAU.has_value() &&
                a.physical.diameterKm.has_value() == p.physical.diameterKm.has_value();
        if (equal && a.physical.absoluteMagnitudeH) {
            equal = near(*a.physical.absoluteMagnitudeH, *p.physical.absoluteMagnitudeH);
        }
    }
    check(equal, "the parsed objects equal the generated ones, unknown values staying unknown");
    check(neo::bench::makeSbdbPageJson(ds, 700, 10).find("\"count\":0") != std::string::npos, "a page past the end is empty, not an error");
}

// --- experiments ---------------------------------------------------------------------------

struct Built {
    neo::Dataset dataset;
    neo::QueryEngine engine;
    explicit Built(neo::Dataset&& ds) : dataset(std::move(ds)), engine(dataset) { engine.build(); }
    Built(const Built&) = delete;
    Built& operator=(const Built&) = delete;
};

neo::bench::ExperimentConfig smallConfig() {
    neo::bench::ExperimentConfig c;
    c.repeats = 5;
    c.lookupQueries = 300;
    c.rangeWindows = 6;
    c.linearBudget = 2000000;
    return c;
}

std::size_t countRows(const neo::bench::Recorder& rec, const std::string& experiment) {
    std::size_t n = 0;
    for (const neo::bench::BenchRow& r : rec.rows()) {
        n += r.experiment == experiment && r.status == "ok" ? 1 : 0;
    }
    return n;
}

void runAll(const neo::bench::DatasetUnderTest& data, const neo::bench::ExperimentConfig& config, neo::bench::Recorder& rec) {
    neo::bench::runLookupExperiment(data, config, rec);
    neo::bench::runRangeExperiment(data, config, rec);
    neo::bench::runMutationExperiment(data, config, rec);
    neo::bench::runTopKExperiment(data, config, rec);
    neo::bench::runQueryExperiment(data, config, rec);
    neo::bench::runMemoryExperiments(data, config, rec);
}

void checkRows(const neo::bench::Recorder& rec, const char* label) {
    bool timed = true, ordered = true, labelled = true, repeatsOk = true;
    for (const neo::bench::BenchRow& r : rec.rows()) {
        if (r.status != "ok") {
            continue;
        }
        labelled = labelled && !r.dataset.empty() && !r.datasetKind.empty() && !r.variant.empty() && !r.unit.empty();
        timed = timed && r.median >= 0.0 && std::isfinite(r.median);
        ordered = ordered && r.min <= r.median + 1e-9 && r.median <= r.max + 1e-9;
        // sizes and derived break-even points are not timings
        if (r.unit != "bytes" && r.experiment != "mutation_breakeven") {
            repeatsOk = repeatsOk && r.repeats >= 5;
        }
    }
    check(labelled, label, "every row names its dataset, kind, variant and unit");
    check(timed && ordered, label, "medians are finite and lie between min and max");
    check(repeatsOk, label, "every timed row has at least 5 repeats");
}

void testExperiments() {
    std::printf("[experiments] every experiment on a small synthetic dataset\n");
    neo::bench::SyntheticOptions options;
    options.objects = 2500;
    options.seed = 21;
    neo::bench::SyntheticInfo info;
    Built built(neo::bench::makeSyntheticDataset(options, &info));

    neo::bench::DatasetUnderTest data;
    data.name = "synthetic-2500";
    data.kind = info.kind;
    data.dataset = &built.dataset;
    data.engine = &built.engine;

    neo::bench::Recorder rec;
    runAll(data, smallConfig(), rec);
    for (const std::string& f : rec.failures()) {
        check(false, "experiment self-check", f);
    }
    check(rec.failures().empty(), "no experiment disagreed with its reference");

    check(countRows(rec, "lookup") == 4, "lookup: linear, dsa::HashMap, std::unordered_map, dsa::IndexedHashMap", std::to_string(countRows(rec, "lookup")));
    check(countRows(rec, "lookup_build") == 3 && countRows(rec, "lookup_memory") == 3, "lookup: three builds, three memory figures");
    check(countRows(rec, "range") == 9, "range: 3 selectivities x 3 variants", std::to_string(countRows(rec, "range")));
    check(countRows(rec, "range_build") == 2 && countRows(rec, "range_memory") == 3, "range: build and memory rows");
    check(countRows(rec, "mutation") == 12 && countRows(rec, "mutation_breakeven") == 3,
          "mutation: 4 per-mutation costs, a rebuild, 5 batch merges, 2 queries; and 3 break-even rows",
          std::to_string(countRows(rec, "mutation")) + "/" + std::to_string(countRows(rec, "mutation_breakeven")));
    check(countRows(rec, "topk") == 15, "topk: 3 values of k x 5 variants", std::to_string(countRows(rec, "topk")));
    check(countRows(rec, "query") == 24, "query: 8 queries x 3 execution modes", std::to_string(countRows(rec, "query")));
    check(countRows(rec, "index_build") > 10 && countRows(rec, "index_memory") > 10, "memory: every index has a build time and a size");
    check(countRows(rec, "master_memory") == 3, "memory: the master vectors");
    checkRows(rec, "[experiments]");

    // the result column is the cross-variant agreement made visible: equal inside each group
    std::set<std::string> groups;
    std::map<std::string, std::uint64_t> firstResult;
    bool equalWithinGroup = true;
    for (const neo::bench::BenchRow& r : rec.rows()) {
        if ((r.experiment != "lookup" && r.experiment != "range" && r.experiment != "topk" && r.experiment != "query") ||
            !r.hasResult) {
            continue;
        }
        const std::string key = r.experiment + "|" + r.param;
        const auto it = firstResult.find(key);
        if (it == firstResult.end()) {
            firstResult[key] = r.result;
        } else {
            equalWithinGroup = equalWithinGroup && it->second == r.result;
        }
    }
    check(equalWithinGroup && !firstResult.empty(), "variants of one group carry the same result checksum in the CSV");

    // mutation rows: every figure positive and finite, break-even rows say what they mean
    bool mutationSane = true, breakevenNamed = true;
    for (const neo::bench::BenchRow& r : rec.rows()) {
        if (r.experiment == "mutation") {
            mutationSane = mutationSane && r.median > 0.0 && std::isfinite(r.median) && r.unit == "ns/op";
        }
        if (r.experiment == "mutation_breakeven") {
            breakevenNamed = breakevenNamed && r.unit == "range queries per mutation" && !r.note.empty() && r.median >= 0.0;
        }
    }
    check(mutationSane && breakevenNamed, "mutation figures are positive and finite; break-even rows carry their unit and explanation");

    // memory: the indexed map really is smaller than the string-keyed one
    double classicBytes = 0, indexedBytes = 0;
    for (const neo::bench::BenchRow& r : rec.rows()) {
        if (r.experiment == "lookup_memory" && r.variant == "dsa::HashMap<string,uint32>") {
            classicBytes = r.median;
        }
        if (r.experiment == "lookup_memory" && r.variant.rfind("dsa::IndexedHashMap", 0) == 0) {
            indexedBytes = r.median;
        }
    }
    check(classicBytes > 0 && indexedBytes > 0 && indexedBytes * 3 < classicBytes, "the record-index map is under a third of the string-keyed table",
          std::to_string(indexedBytes) + " vs " + std::to_string(classicBytes));

    // a dataset too small for an experiment is recorded as not run, not silently skipped
    neo::bench::SyntheticOptions tiny;
    tiny.objects = 3;
    tiny.seed = 1;
    Built small(neo::bench::makeSyntheticDataset(tiny));
    neo::bench::DatasetUnderTest tinyData = data;
    tinyData.name = "synthetic-3";
    tinyData.dataset = &small.dataset;
    tinyData.engine = &small.engine;
    neo::bench::Recorder tinyRec;
    runAll(tinyData, smallConfig(), tinyRec);
    bool anyNotRun = false;
    for (const neo::bench::BenchRow& r : tinyRec.rows()) {
        anyNotRun = anyNotRun || (r.status == "not_run" && !r.note.empty());
    }
    check(anyNotRun && tinyRec.failures().empty(), "a dataset that is too small is recorded as not run, with the reason");
}

void testIngestExperiment() {
    std::printf("[experiments] ingestion by page size\n");
    neo::bench::Recorder rec;
    neo::bench::runIngestExperiment(smallConfig(), 900, {100, 300, 900}, rec);
    check(rec.failures().empty(), "the parser accepted every generated object at every page size");
    check(countRows(rec, "ingest") == 3, "one row per page size");
    bool noted = true;
    for (const neo::bench::BenchRow& r : rec.rows()) {
        noted = noted && r.note.find("PARSE ONLY") != std::string::npos && r.unit == "ns/object" && r.datasetKind == "synthetic-parametric";
    }
    check(noted, "every ingest row says it is parse-only and synthetic");
    checkRows(rec, "[ingest]");
}

void testSummary() {
    std::printf("[summary] markdown report\n");
    neo::bench::SyntheticOptions options;
    options.objects = 800;
    options.seed = 2;
    neo::bench::SyntheticInfo info;
    Built built(neo::bench::makeSyntheticDataset(options, &info));
    neo::bench::DatasetUnderTest data;
    data.name = "synthetic-800";
    data.kind = info.kind;
    data.dataset = &built.dataset;
    data.engine = &built.engine;
    neo::bench::Recorder rec;
    neo::bench::runLookupExperiment(data, smallConfig(), rec);
    rec.notRun("all experiments", "real", "real", "neo.db not found");

    neo::bench::RunInfo run;
    run.compiler = neo::bench::compilerString();
    run.buildType = neo::bench::buildTypeString();
    run.platform = neo::bench::platformString();
    run.startedUtc = "2026-10-03T00:00:00Z";
    run.repeats = 5;
    run.seed = 1;
    run.datasets.push_back({"real (all NEOs)", "real", 0, 0, "not_run: neo.db not found"});
    run.datasets.push_back({"synthetic-800", info.kind, built.dataset.objectCount(), built.dataset.approachCount(), "ok"});
    const std::string md = neo::bench::summaryMarkdown(rec, run);
    check(md.find("Synthetic data is not the real catalogue") != std::string::npos, "synthetic data is called out as such");
    check(md.find("NOT RUN") != std::string::npos && md.find("neo.db not found") != std::string::npos, "a missing real dataset is shown as NOT RUN with the reason");
    check(md.find("dsa::IndexedHashMap") != std::string::npos && md.find("1.00x") != std::string::npos, "variants and their ratio to the first appear");
    check(md.find("identical answers") != std::string::npos, "a clean run says every variant agreed");

    rec.fail("lookup / synthetic-800: the lookup variants returned different records");
    const std::string bad = neo::bench::summaryMarkdown(rec, run);
    check(bad.find("FAILURES") != std::string::npos && bad.find("not trustworthy") != std::string::npos &&
              bad.find("returned different records") != std::string::npos,
          "a correctness failure is stated in the report");

    const std::string path = (std::filesystem::temp_directory_path() / "neo_bench_tests.csv").string();
    std::string error;
    check(rec.writeCsv(path, error), "the CSV is written", error);
    std::ifstream file(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    check(content == rec.csv(), "and reads back exactly");
    std::filesystem::remove(path);
    check(!rec.writeCsv("/nonexistent-dir-for-neo-bench/x.csv", error) && !error.empty(), "an unwritable path is an error, not a crash");
}

void testRealDataset() {
    std::printf("[real] experiments on data/neo.db, if it exists\n");
    const std::string path = "data/neo.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  skipped: no neo.db (run neo_ingest to create one)\n");
        return;
    }
    neo::Dataset ds;
    neo::DatabaseMeta meta;
    const neo::DbStatus status = neo::loadDatabase(path, ds, meta);
    check(status.ok, "the real database loads", status.error);
    if (!status.ok) {
        return;
    }
    Built built(neo::bench::prefixOfDataset(ds, ds.objectCount()));
    neo::bench::DatasetUnderTest data;
    data.name = "real";
    data.kind = "real";
    data.dataset = &built.dataset;
    data.engine = &built.engine;
    neo::bench::Recorder rec;
    runAll(data, smallConfig(), rec);
    for (const std::string& f : rec.failures()) {
        check(false, "real-data experiment self-check", f);
    }
    check(rec.failures().empty(), "no experiment disagreed with its reference on the real data");
    check(countRows(rec, "query") == 24, "all 24 query timings ran on the real data");
    checkRows(rec, "[real]");
}

} // namespace

int main() {
    testTiming();
    testCsv();
    testDesignations();
    testParametric();
    testResampled();
    testSbdbPages();
    testExperiments();
    testIngestExperiment();
    testSummary();
    testRealDataset();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
