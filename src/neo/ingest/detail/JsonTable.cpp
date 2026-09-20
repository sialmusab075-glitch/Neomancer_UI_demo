#include "neo/ingest/detail/JsonTable.h"

#include <cstdio>
#include <cstdlib>

namespace neo {
namespace detail {

namespace {

// JPL sends numbers as strings ("0.0355994"), except SBDB's spkid, which is a
// real JSON number. Both are accepted; anything else is "not a number".
bool toDouble(const nlohmann::json& value, double& out) {
    if (value.is_number()) {
        out = value.get<double>();
        return true;
    }
    if (!value.is_string()) {
        return false;
    }
    const std::string s = value.get<std::string>();
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) {
        return false;
    }
    while (end != nullptr && *end == ' ') {
        ++end;
    }
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

} // namespace

ParseStatus JsonTable::parse(const std::string& text, const char* expectedVersion, const char* apiLabel,
                             const char* docUrl, ValidationReport& report) {
    doc_ = nlohmann::json();
    rows_ = nullptr;
    fields_.clear();
    declaredCount_ = 0;

    doc_ = nlohmann::json::parse(text, nullptr, false); // non-throwing
    if (doc_.is_discarded()) {
        return ParseStatus::failure(std::string(apiLabel) + ": response is not valid JSON");
    }
    if (!doc_.is_object()) {
        return ParseStatus::failure(std::string(apiLabel) + ": response is not a JSON object");
    }
    // An error document ({"code":"400","message":"..."}) carries no data.
    if (doc_.contains("message") && !doc_.contains("data")) {
        const std::string code = doc_.value("code", std::string("?"));
        return ParseStatus::failure(std::string(apiLabel) + ": API error " + code + ": " +
                                    doc_.value("message", std::string("(no message)")));
    }

    // Signature check. JPL states that a version other than the documented one
    // means the response format is not guaranteed, so this is fatal by design:
    // silently parsing an unknown format is how wrong data gets into a database.
    const auto sig = doc_.find("signature");
    if (sig == doc_.end() || !sig->is_object()) {
        return ParseStatus::failure(std::string(apiLabel) + ": response has no signature object");
    }
    report.signatureSource = sig->value("source", std::string());
    report.signatureVersion = sig->value("version", std::string());
    if (report.signatureVersion.empty()) {
        return ParseStatus::failure(std::string(apiLabel) + ": signature has no version");
    }
    if (report.signatureVersion != expectedVersion) {
        return ParseStatus::failure(std::string(apiLabel) + ": unsupported API signature version '" +
                                    report.signatureVersion + "' (this build parses version '" + expectedVersion +
                                    "'). The response format is not guaranteed to match; see " + docUrl);
    }

    double count = 0.0;
    if (doc_.contains("count") && toDouble(doc_["count"], count) && count >= 0.0) {
        declaredCount_ = static_cast<std::size_t>(count);
    }

    const auto fields = doc_.find("fields");
    if (fields == doc_.end()) {
        // A count-only response (CAD total-only=true, or SBDB with no fields
        // requested) is valid and simply has no rows.
        report.rowsSeen = 0;
        return ParseStatus::success();
    }
    if (!fields->is_array()) {
        return ParseStatus::failure(std::string(apiLabel) + ": 'fields' is not an array");
    }
    fields_.reserve(fields->size());
    for (const auto& f : *fields) {
        fields_.push_back(f.is_string() ? f.get<std::string>() : std::string());
    }

    const auto data = doc_.find("data");
    if (data == doc_.end() || data->is_null()) {
        report.rowsSeen = 0;
        return ParseStatus::success();
    }
    if (!data->is_array()) {
        return ParseStatus::failure(std::string(apiLabel) + ": 'data' is not an array");
    }
    rows_ = &(*data);
    report.rowsSeen = rows_->size();
    countNulls(report);
    return ParseStatus::success();
}

void JsonTable::countNulls(ValidationReport& report) const {
    report.nullCounts.clear();
    report.nullCounts.reserve(fields_.size());
    for (std::size_t c = 0; c < fields_.size(); ++c) {
        std::size_t nulls = 0;
        for (std::size_t r = 0; r < rowCount(); ++r) {
            const nlohmann::json& row = (*rows_)[r];
            if (!row.is_array() || c >= row.size() || row[c].is_null()) {
                ++nulls;
            }
        }
        report.nullCounts.emplace_back(fields_[c], nulls);
    }
}

int JsonTable::column(const char* name) const {
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool JsonTable::isNull(const nlohmann::json& row, int col) {
    if (col < 0 || !row.is_array() || static_cast<std::size_t>(col) >= row.size()) {
        return true;
    }
    return row[static_cast<std::size_t>(col)].is_null();
}

std::optional<std::string> JsonTable::text(const nlohmann::json& row, int col) {
    if (isNull(row, col)) {
        return std::nullopt;
    }
    const nlohmann::json& v = row[static_cast<std::size_t>(col)];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number()) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", v.get<double>());
        return std::string(buf);
    }
    return std::nullopt;
}

std::optional<double> JsonTable::number(const nlohmann::json& row, int col) {
    if (isNull(row, col)) {
        return std::nullopt;
    }
    double out = 0.0;
    if (!toDouble(row[static_cast<std::size_t>(col)], out)) {
        return std::nullopt;
    }
    return out;
}

std::optional<int> JsonTable::integer(const nlohmann::json& row, int col) {
    const std::optional<double> v = number(row, col);
    if (!v) {
        return std::nullopt;
    }
    return static_cast<int>(*v);
}

std::optional<bool> JsonTable::yesNo(const nlohmann::json& row, int col) {
    const std::optional<std::string> s = text(row, col);
    if (!s || s->empty()) {
        return std::nullopt;
    }
    const char c = (*s)[0];
    if (c == 'Y' || c == 'y' || c == '1' || c == 'T' || c == 't') {
        return true;
    }
    if (c == 'N' || c == 'n' || c == '0' || c == 'F' || c == 'f') {
        return false;
    }
    return std::nullopt;
}

} // namespace detail
} // namespace neo
