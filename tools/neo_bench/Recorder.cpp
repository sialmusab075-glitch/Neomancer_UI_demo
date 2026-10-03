#include "Recorder.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace neo {
namespace bench {

std::string csvField(const std::string& text) {
    if (text.find_first_of(",\"\n\r") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    out += '"';
    return out;
}

void Recorder::notRun(const std::string& experiment, const std::string& dataset, const std::string& datasetKind,
                      const std::string& reason) {
    BenchRow row;
    row.experiment = experiment;
    row.dataset = dataset;
    row.datasetKind = datasetKind;
    row.status = "not_run";
    row.note = reason;
    rows_.push_back(std::move(row));
}

namespace {

std::string number(double value) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6g", value);
    return buf;
}

} // namespace

std::string Recorder::csv() const {
    std::ostringstream out;
    out << "experiment,dataset,dataset_kind,variant,param,n,ops_per_repeat,repeats,median,min,max,unit,result,status,note\n";
    for (const BenchRow& r : rows_) {
        out << csvField(r.experiment) << ',' << csvField(r.dataset) << ',' << csvField(r.datasetKind) << ','
            << csvField(r.variant) << ',' << csvField(r.param) << ',' << r.n << ',' << r.opsPerRepeat << ','
            << r.repeats << ',';
        if (r.status == "ok") {
            out << number(r.median) << ',' << number(r.min) << ',' << number(r.max);
        } else {
            out << ",,";
        }
        out << ',' << csvField(r.unit) << ',';
        if (r.hasResult) {
            out << r.result;
        }
        out << ',' << csvField(r.status) << ',' << csvField(r.note) << '\n';
    }
    return out.str();
}

bool Recorder::writeCsv(const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open " + path + " for writing";
        return false;
    }
    file << csv();
    file.flush();
    if (!file) {
        error = "write to " + path + " failed";
        return false;
    }
    return true;
}

} // namespace bench
} // namespace neo
