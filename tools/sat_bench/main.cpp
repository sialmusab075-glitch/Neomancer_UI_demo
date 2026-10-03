// sat_bench: what does propagating satellites cost? The answer decides how many the display can afford
// (the plan: a few hundred first, Starlink only after measuring).
//
//   sat_bench --tle-file tests/fixtures/sat/celestrak_3le_sample.txt
//   sat_bench --catalog data/sat/elements.json
//   sat_bench --vallado tests/fixtures/sgp4/SGP4-VER.TLE      (33 real element sets, near-Earth and deep space)
//
// The element sets are copied up to each count, so the cost is that of propagating N DIFFERENT objects (each with
// its own Propagator and state) even though the orbits repeat. Near-Earth and deep-space satellites are reported
// separately: SGP4's deep-space branch is much more expensive. The report says what the input was.

#include "sat/model/ElementSet.h"
#include "sat/parse/TleParser.h"
#include "sat/sim/Propagator.h"
#include "sat/store/Catalog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    return v.size() % 2 == 1 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
}

// Vallado's verification file: line 2 carries three extra fields after column 69, and three satellites
// have bad checksums; both are how the file is published.
std::vector<sat::ElementSet> loadVallado(const std::string& path) {
    std::vector<sat::ElementSet> sets;
    std::istringstream in(readFile(path));
    std::string line;
    sat::TleParseOptions lax;
    lax.requireChecksum = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.rfind("1 ", 0) != 0) {
            continue;
        }
        std::string second;
        std::getline(in, second);
        sat::ElementSet s;
        std::string error;
        if (sat::parseTle("", line, second.substr(0, 69), s, error, lax)) {
            sets.push_back(s);
        }
    }
    return sets;
}

// Distinct elements to keep working: those SGP4 accepts and does not report as decayed within a day.
std::vector<sat::ElementSet> usable(const std::vector<sat::ElementSet>& sets) {
    std::vector<sat::ElementSet> kept;
    for (const sat::ElementSet& s : sets) {
        sat::Propagator p;
        std::string error;
        sat::Propagator::State st;
        if (p.init(s, error) && p.propagateMinutes(0.0, st) == 0 && p.propagateMinutes(1440.0, st) == 0) {
            kept.push_back(s);
        }
    }
    return kept;
}

struct Group {
    std::vector<sat::ElementSet> sets;
};

// Median over `repeats` of the time to propagate `count` satellites (cycling through `sets`) at `frames`
// consecutive instants, as nanoseconds per propagation.
double nsPerPropagation(const std::vector<sat::ElementSet>& sets, std::size_t count, int frames, int repeats, double& checksum,
                        std::size_t& errors) {
    std::vector<sat::Propagator> props(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::string error;
        props[i].init(sets[i % sets.size()], error);
    }
    std::vector<double> samples;
    errors = 0;
    for (int r = -1; r < repeats; ++r) { // r = -1 is the untimed warm-up
        sat::Propagator::State st;
        double sum = 0.0;
        std::size_t failed = 0;
        const Clock::time_point start = Clock::now();
        for (int f = 0; f < frames; ++f) {
            const double minutes = 17.0 * f; // a different instant each frame
            for (std::size_t i = 0; i < count; ++i) {
                if (props[i].propagateMinutes(minutes, st) == 0) {
                    sum += st.r[0] + st.r[1] + st.r[2];
                } else {
                    ++failed;
                }
            }
        }
        const Clock::time_point stop = Clock::now();
        checksum = sum;
        errors = failed;
        if (r >= 0) {
            samples.push_back(std::chrono::duration<double, std::nano>(stop - start).count() / (static_cast<double>(count) * frames));
        }
    }
    return median(samples);
}

} // namespace

int main(int argc, char** argv) {
    std::string tleFile, catalog, vallado;
    int repeats = 7;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::printf("error: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--tle-file") {
            tleFile = value("--tle-file");
        } else if (arg == "--catalog") {
            catalog = value("--catalog");
        } else if (arg == "--vallado") {
            vallado = value("--vallado");
        } else if (arg == "--repeats") {
            repeats = std::max(5, std::atoi(value("--repeats").c_str()));
        } else {
            std::printf("usage: sat_bench (--tle-file F | --catalog F | --vallado F) [--repeats N]\n");
            return arg == "-h" || arg == "--help" ? 0 : 2;
        }
    }
    std::vector<sat::ElementSet> input;
    std::string source;
    if (!catalog.empty()) {
        sat::CatalogMeta meta;
        std::string error;
        if (!sat::loadCatalog(catalog, input, meta, error)) {
            std::printf("error: %s\n", error.c_str());
            return 1;
        }
        source = "catalogue " + catalog;
    } else if (!tleFile.empty()) {
        sat::parseTleText(readFile(tleFile), input);
        source = "TLE file " + tleFile;
    } else if (!vallado.empty()) {
        input = loadVallado(vallado);
        source = "Vallado verification set " + vallado;
    } else {
        std::printf("usage: sat_bench (--tle-file F | --catalog F | --vallado F) [--repeats N]\n");
        return 2;
    }
    const std::vector<sat::ElementSet> sets = usable(input);
    std::vector<sat::ElementSet> nearEarth, deep;
    for (const sat::ElementSet& s : sets) {
        (s.deepSpace() ? deep : nearEarth).push_back(s);
    }
    std::printf("sat_bench: %s\n  %zu element sets read, %zu usable (%zu near-Earth, %zu deep-space), %d repeats, median\n",
#if defined(NDEBUG)
                source.c_str(), input.size(), sets.size(), nearEarth.size(), deep.size(), repeats);
    std::printf("  build: optimised\n");
#else
                source.c_str(), input.size(), sets.size(), nearEarth.size(), deep.size(), repeats);
    std::printf("  build: NOT optimised, the figures are not representative\n");
#endif
    if (sets.empty()) {
        std::printf("error: nothing to propagate\n");
        return 1;
    }

    // init cost
    {
        std::vector<double> samples;
        for (int r = 0; r < repeats; ++r) {
            const Clock::time_point start = Clock::now();
            for (int k = 0; k < 20; ++k) {
                for (const sat::ElementSet& s : sets) {
                    sat::Propagator p;
                    std::string error;
                    p.init(s, error);
                }
            }
            const Clock::time_point stop = Clock::now();
            samples.push_back(std::chrono::duration<double, std::micro>(stop - start).count() / (20.0 * static_cast<double>(sets.size())));
        }
        std::printf("\ninit (sgp4init): %.2f us per satellite\n", median(samples));
    }

    std::printf("\n%-12s %10s %16s %14s %16s\n", "kind", "objects", "ns/propagation", "ms/frame", "% of a 60 fps frame");
    double sink = 0.0;
    auto report = [&](const char* kind, const std::vector<sat::ElementSet>& group) {
        if (group.empty()) {
            return;
        }
        for (const std::size_t count : {std::size_t(100), std::size_t(300), std::size_t(1000), std::size_t(5000), std::size_t(10000)}) {
            const int frames = count >= 5000 ? 20 : 60;
            std::size_t errors = 0;
            const double ns = nsPerPropagation(group, count, frames, repeats, sink, errors);
            const double ms = ns * static_cast<double>(count) / 1.0e6;
            std::printf("%-12s %10zu %16.0f %14.3f %15.1f%%%s\n", kind, count, ns, ms, ms / (1000.0 / 60.0) * 100.0,
                        errors > 0 ? "   (some propagations failed)" : "");
        }
    };
    report("near-Earth", nearEarth);
    report("deep-space", deep);
    std::printf("\n(checksum %.6g, only so the compiler cannot discard the work)\n", sink);
    std::printf("Propagating positions only; converting to Earth-fixed coordinates, drawing and picking are extra.\n");
    return 0;
}
