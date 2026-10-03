#include "sat/parse/OmmJsonParser.h"

#include "sat/model/Epoch.h"
#include "sat/parse/detail/Number.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace sat {

namespace {

using nlohmann::json;

bool getNumber(const json& row, const char* key, double& out, bool required, std::string& error) {
    const auto it = row.find(key);
    if (it == row.end() || it->is_null()) {
        if (required) {
            error = std::string("missing ") + key;
            return false;
        }
        return true;
    }
    if (it->is_number()) {
        out = it->get<double>();
        return true;
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        if (detail::parseDecimal(text.data(), text.size(), out)) {
            return true;
        }
    }
    error = std::string(key) + " is not a number";
    return false;
}

// A whole number that fits an int; a huge or fractional value is an error, never a wrapped cast.
bool getInt(const json& row, const char* key, int& out, std::string& error) {
    double value = 0.0;
    bool present = row.find(key) != row.end() && !row.find(key)->is_null();
    if (!present) {
        return true; // optional field: keep the default
    }
    if (!getNumber(row, key, value, false, error)) {
        return false;
    }
    if (!(value >= -2147483648.0 && value <= 2147483647.0) || value != std::floor(value)) {
        error = std::string(key) + " is not a whole number in range";
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool getText(const json& row, const char* key, std::string& out) {
    const auto it = row.find(key);
    if (it == row.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool toElementSet(const json& row, ElementSet& out, std::string& error) {
    if (!row.is_object()) {
        error = "not a JSON object";
        return false;
    }
    ElementSet s;
    getText(row, "OBJECT_NAME", s.name);
    getText(row, "OBJECT_ID", s.objectId);
    std::string classification;
    if (getText(row, "CLASSIFICATION_TYPE", classification) && !classification.empty()) {
        s.classification = classification[0];
    }

    double id = 0.0;
    if (!getNumber(row, "NORAD_CAT_ID", id, true, error)) {
        return false;
    }
    if (!(id >= 0.0 && id < 4294967295.0) || id != std::floor(id)) {
        error = "NORAD_CAT_ID is not a catalogue number";
        return false;
    }
    s.noradId = static_cast<std::uint32_t>(id);

    std::string epoch;
    if (!getText(row, "EPOCH", epoch)) {
        error = "missing EPOCH";
        return false;
    }
    if (!epochFromIso(epoch, s.epochJdWhole, s.epochJdFraction, error)) {
        return false;
    }

    if (!getNumber(row, "MEAN_MOTION", s.meanMotionRevPerDay, true, error) ||
        !getNumber(row, "ECCENTRICITY", s.eccentricity, true, error) ||
        !getNumber(row, "INCLINATION", s.inclinationDeg, true, error) ||
        !getNumber(row, "RA_OF_ASC_NODE", s.raanDeg, true, error) ||
        !getNumber(row, "ARG_OF_PERICENTER", s.argPerigeeDeg, true, error) ||
        !getNumber(row, "MEAN_ANOMALY", s.meanAnomalyDeg, true, error) || !getNumber(row, "BSTAR", s.bstar, true, error) ||
        !getNumber(row, "MEAN_MOTION_DOT", s.meanMotionDot, true, error) ||
        !getNumber(row, "MEAN_MOTION_DDOT", s.meanMotionDdot, true, error) ||
        !getInt(row, "EPHEMERIS_TYPE", s.ephemerisType, error) || !getInt(row, "ELEMENT_SET_NO", s.elementSetNo, error) ||
        !getInt(row, "REV_AT_EPOCH", s.revAtEpoch, error)) {
        return false;
    }

    if (!validateElementSet(s, error)) {
        return false;
    }
    out = std::move(s);
    return true;
}

std::string excerpt(const std::string& body) {
    std::string out;
    for (const char c : body) {
        if (out.size() >= 80) {
            out += "...";
            break;
        }
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    return out;
}

} // namespace

OmmParseResult parseOmmJson(const std::string& text, std::vector<ElementSet>& out) {
    OmmParseResult result;
    const json doc = json::parse(text, nullptr, false); // non-throwing
    if (doc.is_discarded()) {
        result.error = "not JSON: '" + excerpt(text) + "'";
        return result;
    }
    json single;
    const json* rows = &doc;
    if (doc.is_object()) {
        single = json::array({doc});
        rows = &single;
    }
    if (!rows->is_array()) {
        result.error = "expected a JSON array of OMM records";
        return result;
    }
    result.ok = true;
    std::size_t index = 0;
    for (const json& row : *rows) {
        ++result.report.seen;
        ElementSet set;
        std::string error;
        if (toElementSet(row, set, error)) {
            ++result.report.accepted;
            out.push_back(std::move(set));
        } else {
            std::string label = row.is_object() && row.contains("OBJECT_NAME") && row["OBJECT_NAME"].is_string()
                                    ? row["OBJECT_NAME"].get<std::string>()
                                    : std::string("(unnamed)");
            result.report.reject(index, label + ": " + error);
        }
        ++index;
    }
    return result;
}

} // namespace sat
