#pragma once

#include "neo/ingest/HttpClient.h"

#include <chrono>
#include <string>

namespace neo {

// The only networking code in the project. It lives in its own target
// (solsim_neo_net) that only tools/neo_ingest links, so nothing else can reach
// the network by accident: tests and the UI see IHttpClient and get a fake.
//
// WinHTTP is a Windows system component, so this adds no third-party dependency.
class WinHttpClient : public IHttpClient {
public:
    struct Options {
        std::string userAgent = "SolSystemSim-NEO/0.1 (semester project; C++/WinHTTP)";
        std::chrono::milliseconds connectTimeout{15000};
        std::chrono::milliseconds sendTimeout{15000};
        std::chrono::milliseconds receiveTimeout{60000}; // a full CAD page can be large
    };

    // Two constructors rather than a default argument: a default argument of
    // type Options would need the nested class to be complete inside its own
    // enclosing class definition.
    WinHttpClient();
    explicit WinHttpClient(Options options);
    ~WinHttpClient() override;

    WinHttpClient(const WinHttpClient&) = delete;
    WinHttpClient& operator=(const WinHttpClient&) = delete;

    HttpResponse get(const std::string& url) override;
    const char* userAgent() const override { return options_.userAgent.c_str(); }

    // False when the WinHTTP session could not be opened at all.
    bool valid() const { return session_ != nullptr; }

private:
    Options options_;
    void*   session_ = nullptr; // HINTERNET, kept opaque so <windows.h> stays out of this header
};

} // namespace neo
