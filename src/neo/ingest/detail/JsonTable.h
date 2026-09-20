#pragma once

// Internal helper shared by the SBDB and CAD parsers. It is the only place that
// sees nlohmann/json, and it is included from .cpp files only, so the JSON
// library never reaches the rest of the program (or its compile times).
//
// Both APIs answer in the same shape:
//
//   { "signature": {"source": ..., "version": ...},
//     "count": N,
//     "fields": ["des", "jd", ...],
//     "data":   [ ["2020 AN3", "2458863.7", ...], ... ] }
//
// so column order is decided by the response, never assumed. Every value is a
// JSON string or null, except SBDB's spkid, which is a number.

#include "neo/ingest/ParseReport.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace neo {
namespace detail {

class JsonTable {
public:
    // Parses `text`, checks signature.version against `expectedVersion`, and
    // fills the report's signature fields and per-column null counts.
    // `apiLabel` and `docUrl` only shape the error messages.
    ParseStatus parse(const std::string& text, const char* expectedVersion, const char* apiLabel,
                      const char* docUrl, ValidationReport& report);

    // Index of a column, or -1 when the response does not carry it (the CAD
    // diameter columns only exist when the request asked for them).
    int column(const char* name) const;

    std::size_t rowCount() const { return rows_ == nullptr ? 0u : rows_->size(); }
    const nlohmann::json& row(std::size_t i) const { return (*rows_)[i]; }
    const std::vector<std::string>& fields() const { return fields_; }
    std::size_t declaredCount() const { return declaredCount_; }

    // Cell accessors. `col` may be -1 (absent column) and the row may be short;
    // both are treated as "value not present", never as an error or a zero.
    static bool isNull(const nlohmann::json& row, int col);
    static std::optional<std::string> text(const nlohmann::json& row, int col);
    static std::optional<double> number(const nlohmann::json& row, int col);
    static std::optional<int> integer(const nlohmann::json& row, int col);
    // "Y" -> true, "N" -> false, null/absent -> empty (SBDB neo / pha flags).
    static std::optional<bool> yesNo(const nlohmann::json& row, int col);

private:
    void countNulls(ValidationReport& report) const;

    nlohmann::json           doc_;
    const nlohmann::json*    rows_ = nullptr;
    std::vector<std::string> fields_;
    std::size_t              declaredCount_ = 0;
};

} // namespace detail
} // namespace neo
