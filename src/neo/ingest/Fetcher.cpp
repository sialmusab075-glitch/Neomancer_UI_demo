#include "neo/ingest/Fetcher.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <utility>

namespace neo {

namespace {

std::string shortUrl(const std::string& url) {
    // Enough of the query to identify the page in a log line.
    constexpr std::size_t kMax = 110;
    return url.size() <= kMax ? url : url.substr(0, kMax) + "...";
}

} // namespace

Fetcher::Fetcher(IHttpClient& http, ResponseCache& cache, CacheMode mode, FetchPolicy policy)
    : http_(http), cache_(cache), mode_(mode), policy_(policy),
      sleep_([](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }), rng_(policy.jitterSeed) {}

void Fetcher::logLine(const std::string& text) const {
    if (log_) {
        log_(text);
    }
}

void Fetcher::waitFor(std::chrono::milliseconds ms) {
    if (ms.count() <= 0) {
        return;
    }
    stats_.waitTime += ms;
    if (sleep_) {
        sleep_(ms);
    }
}

std::chrono::milliseconds Fetcher::backoffFor(int attempt) {
    // Exponential: base * 2^(attempt-1), capped, plus up to 50% jitter so a
    // retry storm cannot synchronise with anything else hitting the same host.
    double ms = static_cast<double>(policy_.baseBackoff.count());
    for (int i = 1; i < attempt; ++i) {
        ms *= 2.0;
    }
    ms = std::min(ms, static_cast<double>(policy_.maxBackoff.count()));
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const double jitter = 0.5 * ms * (static_cast<double>(rng_ >> 8) / 16777216.0);
    return std::chrono::milliseconds(static_cast<long long>(ms + jitter));
}

Fetcher::Result Fetcher::get(const std::string& url, bool preferCache) {
    Result result;

    const bool useCache = mode_ != CacheMode::Refresh || preferCache;
    if (useCache && cache_.load(url, result.body)) {
        result.ok = true;
        result.fromCache = true;
        ++stats_.cacheHits;
        stats_.bytesFromCache += result.body.size();
        logLine("cache  " + shortUrl(url));
        return result;
    }
    if (mode_ == CacheMode::Offline) {
        result.error = "offline: no cached response for " + url;
        ++stats_.failures;
        return result;
    }

    std::string lastError;
    for (int attempt = 1; attempt <= policy_.maxAttempts; ++attempt) {
        // Stay at least minInterval apart, measured from the previous request.
        if (anyRequest_) {
            const auto since = std::chrono::steady_clock::now() - lastRequest_;
            const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(since);
            if (sinceMs < policy_.minInterval) {
                waitFor(policy_.minInterval - sinceMs);
            }
        }

        const auto started = std::chrono::steady_clock::now();
        const HttpResponse response = http_.get(url);
        const auto finished = std::chrono::steady_clock::now();
        lastRequest_ = finished;
        anyRequest_ = true;
        ++stats_.requests;
        stats_.networkTime += std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        stats_.bytesDownloaded += response.body.size();

        if (response.ok()) {
            result.ok = true;
            result.body = response.body;
            cache_.store(url, response.body, response.status);
            logLine("get    " + shortUrl(url) + "  (" + std::to_string(response.body.size()) + " B)");
            return result;
        }

        char buf[192];
        if (response.transportFailure()) {
            std::snprintf(buf, sizeof buf, "%s%s", response.timedOut ? "timeout: " : "transport error: ",
                          response.error.c_str());
        } else {
            std::snprintf(buf, sizeof buf, "HTTP %d", response.status);
        }
        lastError = buf;

        if (!response.retryable()) {
            // 4xx: the request itself is wrong. Retrying only wastes JPL's time.
            result.error = lastError + " for " + url;
            ++stats_.failures;
            logLine("fail   " + lastError + "  " + shortUrl(url));
            return result;
        }
        if (attempt < policy_.maxAttempts) {
            const auto backoff = backoffFor(attempt);
            ++stats_.retries;
            logLine("retry  " + lastError + " -> waiting " + std::to_string(backoff.count()) + " ms (attempt " +
                    std::to_string(attempt + 1) + "/" + std::to_string(policy_.maxAttempts) + ")");
            waitFor(backoff);
        }
    }

    result.error = lastError + " after " + std::to_string(policy_.maxAttempts) + " attempts for " + url;
    ++stats_.failures;
    logLine("fail   " + result.error);
    return result;
}

} // namespace neo
