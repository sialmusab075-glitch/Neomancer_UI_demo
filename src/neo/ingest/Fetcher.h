#pragma once

#include "neo/ingest/HttpClient.h"
#include "neo/ingest/ResponseCache.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace neo {

// How the cache is used for this run.
enum class CacheMode {
    Normal,  // serve from the cache when present, otherwise fetch and store
    Offline, // never touch the network; a cache miss is an error
    Refresh, // ignore what is cached, fetch again and overwrite
};

// Politeness towards JPL. The defaults are deliberately conservative: one
// request at a time, at least a second apart, and at most five attempts.
struct FetchPolicy {
    std::chrono::milliseconds minInterval{1000}; // enforced between requests
    std::chrono::milliseconds baseBackoff{1000}; // 1s, 2s, 4s, 8s (doubling)
    std::chrono::milliseconds maxBackoff{30000};
    int          maxAttempts = 5;
    std::uint32_t jitterSeed = 0x5EEDu; // seeded, so a run is reproducible
};

struct FetchStats {
    std::size_t requests = 0;    // network requests actually issued
    std::size_t cacheHits = 0;
    std::size_t retries = 0;     // repeat attempts after a retryable failure
    std::size_t failures = 0;    // URLs given up on after maxAttempts
    std::size_t bytesDownloaded = 0;
    std::size_t bytesFromCache = 0;
    std::chrono::milliseconds networkTime{0};
    std::chrono::milliseconds waitTime{0}; // spent being polite (interval + backoff)
};

// Serialises every request, keeps them a minimum interval apart, retries only
// what is worth retrying, and reads/writes the raw response cache.
class Fetcher {
public:
    struct Result {
        bool        ok = false;
        std::string body;
        std::string error;
        bool        fromCache = false;
    };

    Fetcher(IHttpClient& http, ResponseCache& cache, CacheMode mode, FetchPolicy policy = FetchPolicy());

    // `preferCache` serves an entry from the cache even in Refresh mode: used
    // when the progress file says this run already refreshed that URL, so a
    // resumed --refresh run does not download the same pages twice.
    Result get(const std::string& url, bool preferCache = false);

    const FetchStats& stats() const { return stats_; }
    CacheMode mode() const { return mode_; }

    // Tests replace the sleep with a no-op so backoff costs no wall-clock time;
    // the waited durations are still accumulated in the stats.
    using SleepFn = std::function<void(std::chrono::milliseconds)>;
    void setSleepFunction(SleepFn fn) { sleep_ = std::move(fn); }

    // Called with a human-readable line for each attempt, retry and cache hit.
    using LogFn = std::function<void(const std::string&)>;
    void setLogFunction(LogFn fn) { log_ = std::move(fn); }

private:
    void logLine(const std::string& text) const;
    void waitFor(std::chrono::milliseconds ms);
    std::chrono::milliseconds backoffFor(int attempt);

    IHttpClient&   http_;
    ResponseCache& cache_;
    CacheMode      mode_;
    FetchPolicy    policy_;
    FetchStats     stats_;
    SleepFn        sleep_;
    LogFn          log_;
    std::uint32_t  rng_;
    std::chrono::steady_clock::time_point lastRequest_{};
    bool           anyRequest_ = false;
};

} // namespace neo
