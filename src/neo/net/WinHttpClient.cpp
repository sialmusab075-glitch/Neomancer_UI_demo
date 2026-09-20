#include "neo/net/WinHttpClient.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <utility>
#include <vector>

namespace neo {

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], needed);
    return out;
}

std::string lastErrorText(const char* what) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s (WinHTTP error %lu)", what, static_cast<unsigned long>(GetLastError()));
    return buf;
}

// Closes a handle when the scope ends; WinHTTP has three nested ones per request.
struct HandleGuard {
    HINTERNET handle = nullptr;
    ~HandleGuard() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

} // namespace

WinHttpClient::WinHttpClient() : WinHttpClient(Options()) {}

WinHttpClient::WinHttpClient(Options options) : options_(std::move(options)) {
    const std::wstring agent = widen(options_.userAgent);
    session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_ != nullptr) {
        WinHttpSetTimeouts(session_, static_cast<int>(options_.connectTimeout.count()),
                           static_cast<int>(options_.connectTimeout.count()),
                           static_cast<int>(options_.sendTimeout.count()),
                           static_cast<int>(options_.receiveTimeout.count()));
    }
}

WinHttpClient::~WinHttpClient() {
    if (session_ != nullptr) {
        WinHttpCloseHandle(session_);
        session_ = nullptr;
    }
}

HttpResponse WinHttpClient::get(const std::string& url) {
    HttpResponse response;
    if (session_ == nullptr) {
        response.error = "WinHTTP session could not be opened";
        return response;
    }

    const std::wstring wideUrl = widen(url);
    URL_COMPONENTS parts;
    ZeroMemory(&parts, sizeof parts);
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        response.error = lastErrorText("malformed URL");
        return response;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HandleGuard connection;
    connection.handle = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
    if (connection.handle == nullptr) {
        response.error = lastErrorText("connect failed");
        return response;
    }

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0u;
    HandleGuard request;
    request.handle = WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request.handle == nullptr) {
        response.error = lastErrorText("could not create request");
        return response;
    }

    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        const DWORD code = GetLastError();
        response.timedOut = code == ERROR_WINHTTP_TIMEOUT;
        response.error = lastErrorText(response.timedOut ? "request timed out" : "request failed");
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof status;
    if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        response.status = static_cast<int>(status);
    } else {
        response.error = lastErrorText("could not read status code");
        return response;
    }

    std::vector<char> chunk;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            const DWORD code = GetLastError();
            response.timedOut = code == ERROR_WINHTTP_TIMEOUT;
            response.status = 0; // incomplete body: treat as a transport failure, so it is retried
            response.error = lastErrorText("read failed");
            return response;
        }
        if (available == 0) {
            break;
        }
        chunk.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, chunk.data(), available, &read)) {
            response.status = 0;
            response.error = lastErrorText("read failed");
            return response;
        }
        response.body.append(chunk.data(), read);
    }
    return response;
}

} // namespace neo
