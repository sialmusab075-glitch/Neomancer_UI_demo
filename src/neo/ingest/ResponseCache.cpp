#include "neo/ingest/ResponseCache.h"

#include "neo/model/JulianDate.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace neo {

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

} // namespace

ResponseCache::ResponseCache(std::string directory) : dir_(std::move(directory)) {}

std::string ResponseCache::keyFor(const std::string& url) {
    // FNV-1a, 64-bit.
    std::uint64_t hash = 1469598103934665603ull;
    for (const char c : url) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

std::string ResponseCache::bodyPath(const std::string& url) const { return dir_ + "/" + keyFor(url) + ".json"; }

std::string ResponseCache::metaPath(const std::string& url) const {
    return dir_ + "/" + keyFor(url) + ".meta.json";
}

bool ResponseCache::has(const std::string& url) const {
    std::error_code ec;
    return std::filesystem::exists(bodyPath(url), ec) && !ec;
}

bool ResponseCache::load(const std::string& url, std::string& body) const {
    std::string meta;
    if (!readFile(metaPath(url), meta)) {
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(meta, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("url", std::string()) != url) {
        return false; // absent, corrupt, or a different URL behind the same key
    }
    return readFile(bodyPath(url), body);
}

bool ResponseCache::store(const std::string& url, const std::string& body, int status) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return false;
    }
    if (!writeFile(bodyPath(url), body)) {
        return false;
    }
    nlohmann::json meta;
    meta["url"] = url;
    meta["status"] = status;
    meta["bytes"] = body.size();
    meta["fetched_utc"] = utcNowIso();
    return writeFile(metaPath(url), meta.dump(2));
}

std::size_t ResponseCache::entryCount() const {
    std::error_code ec;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0 &&
            name.find(".meta.") == std::string::npos) {
            ++count;
        }
    }
    return count;
}

} // namespace neo
