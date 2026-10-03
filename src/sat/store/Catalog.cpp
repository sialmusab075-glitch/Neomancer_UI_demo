#include "sat/store/Catalog.h"

#include "sat/model/Epoch.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sat {

namespace {

using nlohmann::json;

json toJson(const ElementSet& s) {
    json j;
    j["OBJECT_NAME"] = s.name;
    j["OBJECT_ID"] = s.objectId;
    j["NORAD_CAT_ID"] = s.noradId;
    j["CLASSIFICATION_TYPE"] = std::string(1, s.classification);
    j["EPOCH"] = epochToIso(s.epochJdWhole, s.epochJdFraction);
    j["EPOCH_JD_WHOLE"] = s.epochJdWhole;
    j["EPOCH_JD_FRACTION"] = s.epochJdFraction;
    j["MEAN_MOTION"] = s.meanMotionRevPerDay;
    j["ECCENTRICITY"] = s.eccentricity;
    j["INCLINATION"] = s.inclinationDeg;
    j["RA_OF_ASC_NODE"] = s.raanDeg;
    j["ARG_OF_PERICENTER"] = s.argPerigeeDeg;
    j["MEAN_ANOMALY"] = s.meanAnomalyDeg;
    j["BSTAR"] = s.bstar;
    j["MEAN_MOTION_DOT"] = s.meanMotionDot;
    j["MEAN_MOTION_DDOT"] = s.meanMotionDdot;
    j["EPHEMERIS_TYPE"] = s.ephemerisType;
    j["ELEMENT_SET_NO"] = s.elementSetNo;
    j["REV_AT_EPOCH"] = s.revAtEpoch;
    return j;
}

bool number(const json& j, const char* key, double& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return false;
    }
    out = it->get<double>();
    return true;
}

bool wholeNumber(double value, double lo, double hi) { return value >= lo && value <= hi && value == std::floor(value); }

// A string member, or "" when it is missing or not a string (json::value() would throw on a type mismatch).
std::string textOr(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool fromJson(const json& j, ElementSet& s) {
    if (!j.is_object()) {
        return false;
    }
    double id = 0, eph = 0, setNo = 0, rev = 0;
    std::string classification;
    if (!j.contains("OBJECT_NAME") || !j["OBJECT_NAME"].is_string() || !j.contains("OBJECT_ID") || !j["OBJECT_ID"].is_string() ||
        !j.contains("CLASSIFICATION_TYPE") || !j["CLASSIFICATION_TYPE"].is_string()) {
        return false;
    }
    s.name = j["OBJECT_NAME"].get<std::string>();
    s.objectId = j["OBJECT_ID"].get<std::string>();
    classification = j["CLASSIFICATION_TYPE"].get<std::string>();
    s.classification = classification.empty() ? 'U' : classification[0];
    if (!number(j, "NORAD_CAT_ID", id) || !number(j, "EPOCH_JD_WHOLE", s.epochJdWhole) ||
        !number(j, "EPOCH_JD_FRACTION", s.epochJdFraction) || !number(j, "MEAN_MOTION", s.meanMotionRevPerDay) ||
        !number(j, "ECCENTRICITY", s.eccentricity) || !number(j, "INCLINATION", s.inclinationDeg) ||
        !number(j, "RA_OF_ASC_NODE", s.raanDeg) || !number(j, "ARG_OF_PERICENTER", s.argPerigeeDeg) ||
        !number(j, "MEAN_ANOMALY", s.meanAnomalyDeg) || !number(j, "BSTAR", s.bstar) ||
        !number(j, "MEAN_MOTION_DOT", s.meanMotionDot) || !number(j, "MEAN_MOTION_DDOT", s.meanMotionDdot) ||
        !number(j, "EPHEMERIS_TYPE", eph) || !number(j, "ELEMENT_SET_NO", setNo) || !number(j, "REV_AT_EPOCH", rev)) {
        return false;
    }
    if (!wholeNumber(id, 0.0, 4294967295.0) || !wholeNumber(eph, -2147483648.0, 2147483647.0) ||
        !wholeNumber(setNo, -2147483648.0, 2147483647.0) || !wholeNumber(rev, -2147483648.0, 2147483647.0)) {
        return false;
    }
    s.noradId = static_cast<std::uint32_t>(id);
    s.ephemerisType = static_cast<int>(eph);
    s.elementSetNo = static_cast<int>(setNo);
    s.revAtEpoch = static_cast<int>(rev);
    std::string reason;
    return validateElementSet(s, reason);
}

} // namespace

std::string toOmmJson(const std::vector<ElementSet>& sets) {
    json rows = json::array();
    for (const ElementSet& s : sets) {
        rows.push_back(toJson(s));
    }
    return rows.dump();
}

bool saveCatalog(const std::string& path, const std::vector<ElementSet>& sets, const CatalogMeta& meta, std::string& error) {
    json doc;
    doc["schema"] = meta.schemaVersion;
    doc["created_utc"] = meta.createdUtc;
    doc["source"] = meta.source;
    doc["groups"] = meta.groups;
    json rows = json::array();
    for (const ElementSet& s : sets) {
        rows.push_back(toJson(s));
    }
    doc["sets"] = rows;

    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write " + temp;
            return false;
        }
        file << doc.dump(1) << '\n';
        file.flush();
        if (!file) {
            error = "writing " + temp + " failed";
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        // Windows refuses to rename over an existing file on some filesystems: replace explicitly.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            error = "cannot replace " + path + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool loadCatalog(const std::string& path, std::vector<ElementSet>& sets, CatalogMeta& meta, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const json doc = json::parse(buffer.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        error = path + " is not a satellite catalogue (not a JSON object)";
        return false;
    }
    if (!doc.contains("schema") || !doc["schema"].is_number_integer() || doc["schema"].get<int>() != kCatalogSchemaVersion) {
        error = path + " has schema " + (doc.contains("schema") ? doc["schema"].dump() : std::string("none")) +
                ", this build reads " + std::to_string(kCatalogSchemaVersion) + ": run sat_ingest again";
        return false;
    }
    if (!doc.contains("sets") || !doc["sets"].is_array()) {
        error = path + " has no element sets";
        return false;
    }
    std::vector<ElementSet> loaded;
    loaded.reserve(doc["sets"].size());
    for (const json& row : doc["sets"]) {
        ElementSet s;
        if (!fromJson(row, s)) {
            error = path + " holds a record that is not a valid element set (#" + std::to_string(loaded.size()) + ")";
            return false;
        }
        loaded.push_back(std::move(s));
    }
    CatalogMeta m;
    m.schemaVersion = doc["schema"].get<int>();
    m.createdUtc = textOr(doc, "created_utc");
    m.source = textOr(doc, "source");
    if (doc.contains("groups") && doc["groups"].is_array()) {
        for (const json& g : doc["groups"]) {
            if (g.is_string()) {
                m.groups.push_back(g.get<std::string>());
            }
        }
    }
    sets = std::move(loaded);
    meta = std::move(m);
    return true;
}

} // namespace sat
