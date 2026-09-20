#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace neo {

// Raw responses on disk, so a re-run costs JPL nothing and an interrupted run
// can be resumed offline. Each entry is two files in the cache directory:
//
//   <key>.json       the response body, byte for byte
//   <key>.meta.json  the exact URL, HTTP status, byte count and fetch time
//
// The key is a 64-bit FNV-1a hash of the full URL, hex encoded. It is a cache
// key, not a security primitive; the URL is stored alongside so an entry can
// always be traced back to the request that produced it (and so a hash
// collision, though never observed, would be visible rather than silent).
class ResponseCache {
public:
    explicit ResponseCache(std::string directory);

    const std::string& directory() const { return dir_; }

    static std::string keyFor(const std::string& url);
    std::string bodyPath(const std::string& url) const;
    std::string metaPath(const std::string& url) const;

    bool has(const std::string& url) const;
    // Reads the cached body. Returns false when absent or unreadable, or when
    // the stored URL does not match (a collision or a hand-edited cache).
    bool load(const std::string& url, std::string& body) const;
    // Writes body + metadata. Returns false when the directory cannot be
    // created or written; ingestion continues without caching in that case.
    bool store(const std::string& url, const std::string& body, int status);

    std::size_t entryCount() const;

private:
    std::string dir_;
};

} // namespace neo
