#pragma once

#include <cstddef>
#include <string>

namespace neo {

// One HTTP result. A transport failure (DNS, TLS, timeout, reset) has
// status == 0 and a message in `error`; an HTTP error has the status code and a
// body, because JPL returns its own JSON error documents with 4xx codes.
struct HttpResponse {
    int         status = 0;
    std::string body;
    std::string error;
    bool        timedOut = false;

    bool ok() const { return status == 200; }
    bool transportFailure() const { return status == 0; }
    // 5xx and timeouts are the only things worth retrying: JPL's gateway
    // returns intermittent 502s for perfectly valid queries. A 4xx means the
    // request itself is wrong, so retrying it just wastes JPL's time.
    bool retryable() const { return transportFailure() || (status >= 500 && status < 600); }
};

// The only way the NEO layer reaches the network. The WinHTTP implementation
// lives in a separate target (solsim_neo_net) that only the CLI links, so tests
// and the UI cannot accidentally make a request: they get a fake.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual HttpResponse get(const std::string& url) = 0;
    // Identifies this project to JPL in the User-Agent header.
    virtual const char* userAgent() const = 0;
};

} // namespace neo
