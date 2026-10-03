#include "Summary.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace neo {
namespace bench {

std::string compilerString() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
#else
    return "unknown compiler";
#endif
}

std::string buildTypeString() {
#if defined(NDEBUG)
    return "optimised (NDEBUG)";
#else
    return "DEBUG (assertions on, not representative)";
#endif
}

std::string platformString() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "unknown OS";
#endif
}

namespace {

std::string g4(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

std::string human(double value, const std::string& unit) {
    if (unit == "bytes") {
        char buf[48];
        if (value >= 1048576.0) {
            std::snprintf(buf, sizeof buf, "%.2f MiB", value / 1048576.0);
        } else if (value >= 1024.0) {
            std::snprintf(buf, sizeof buf, "%.1f KiB", value / 1024.0);
        } else {
            std::snprintf(buf, sizeof buf, "%.0f B", value);
        }
        return buf;
    }
    return g4(value) + " " + unit;
}

std::string cell(std::string text) {
    for (char& c : text) {
        if (c == '|') {
            c = '/';
        }
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return text;
}

} // namespace

std::string summaryMarkdown(const Recorder& recorder, const RunInfo& info) {
    std::ostringstream out;
    out << "# neo_bench results\n\n";
    out << "- run started (UTC): " << info.startedUtc << "\n";
    out << "- platform: " << info.platform << ", " << info.threads << " hardware threads\n";
    out << "- compiler: " << info.compiler << ", build: " << info.buildType << "\n";
    out << "- repeats per timing: " << info.repeats << " (median reported, min-max beside it), seed " << info.seed << "\n";
    out << "- real database: " << (info.dbPath.empty() ? "(none)" : info.dbPath) << "\n\n";

    out << "## Datasets\n\n| dataset | kind | objects | approaches | status |\n|---|---|---:|---:|---|\n";
    bool anySynthetic = false;
    for (const DatasetSummary& d : info.datasets) {
        out << "| " << cell(d.name) << " | " << cell(d.kind) << " | " << d.objects << " | " << d.approaches << " | "
            << cell(d.status) << " |\n";
        anySynthetic = anySynthetic || d.kind.rfind("synthetic", 0) == 0;
    }
    if (anySynthetic) {
        out << "\n**Synthetic data is not the real catalogue.** Rows on a `synthetic-*` dataset describe how the structures scale, "
               "not how the real NEO data behaves; the dataset kind is repeated on every CSV row.\n";
    }

    // experiments in the order they were first recorded, grouped by dataset
    std::vector<std::string> datasetOrder;
    std::map<std::string, std::vector<std::string>> experimentOrder;
    for (const BenchRow& r : recorder.rows()) {
        if (std::find(datasetOrder.begin(), datasetOrder.end(), r.dataset) == datasetOrder.end()) {
            datasetOrder.push_back(r.dataset);
        }
        std::vector<std::string>& list = experimentOrder[r.dataset];
        if (std::find(list.begin(), list.end(), r.experiment) == list.end()) {
            list.push_back(r.experiment);
        }
    }

    for (const std::string& dataset : datasetOrder) {
        out << "\n## " << cell(dataset) << "\n";
        for (const std::string& experiment : experimentOrder[dataset]) {
            out << "\n### " << cell(experiment) << "\n\n";
            out << "| param | variant | median | min - max | vs first | note |\n|---|---|---:|---:|---:|---|\n";
            std::map<std::string, double> baseline; // per param
            for (const BenchRow& r : recorder.rows()) {
                if (r.dataset != dataset || r.experiment != experiment) {
                    continue;
                }
                if (r.status != "ok") {
                    out << "| " << cell(r.param) << " | " << cell(r.variant) << " | NOT RUN | | | " << cell(r.note) << " |\n";
                    continue;
                }
                std::string ratio = "-";
                const auto it = baseline.find(r.param);
                if (it == baseline.end()) {
                    baseline[r.param] = r.median;
                    ratio = "1.00x";
                } else if (it->second > 0.0) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%.2fx", r.median / it->second);
                    ratio = buf;
                }
                out << "| " << cell(r.param) << " | " << cell(r.variant) << " | " << human(r.median, r.unit) << " | "
                    << (r.repeats > 1 ? human(r.min, r.unit) + " - " + human(r.max, r.unit) : std::string("-")) << " | " << ratio
                    << " | " << cell(r.note) << " |\n";
            }
        }
    }

    out << "\n## Correctness checks\n\n";
    if (recorder.failures().empty()) {
        out << "Every experiment's variants returned identical answers (the figures above compare equal work).\n";
    } else {
        out << "**FAILURES: the figures above are not trustworthy.**\n\n";
        for (const std::string& f : recorder.failures()) {
            out << "- " << f << "\n";
        }
    }
    out << "\n`vs first` is the median divided by the first variant of the same parameter (below 1.00x is faster or smaller).\n";
    return out.str();
}

} // namespace bench
} // namespace neo
