#include "app/Paths.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <vector>

namespace app {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), &out[0], n,
                        nullptr, nullptr);
    return out;
}

std::string executableDir() {
    static const std::string cached = [] {
        std::vector<wchar_t> buf(MAX_PATH);
        for (;;) {
            const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (len == 0) {
                return std::string(".");
            }
            if (len < buf.size()) {
                std::wstring path(buf.data(), len);
                const std::size_t slash = path.find_last_of(L"\\/");
                if (slash != std::wstring::npos) {
                    path.resize(slash);
                }
                return narrow(path);
            }
            buf.resize(buf.size() * 2); // path longer than the buffer; retry
        }
    }();
    return cached;
}

std::string assetPath(const std::string& relative) {
    return executableDir() + "/assets/" + relative;
}

bool fileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesW(widen(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::FILE* openFile(const std::string& pathUtf8, const wchar_t* mode) {
    const std::wstring wide = widen(pathUtf8);
#ifdef _MSC_VER
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, wide.c_str(), mode) != 0) {
        return nullptr;
    }
    return f;
#else
    return _wfopen(wide.c_str(), mode);
#endif
}

std::string envVar(const char* name) {
    const std::wstring wname = widen(name);
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) {
        return std::string();
    }
    std::wstring value(n, L'\0');
    const DWORD written = GetEnvironmentVariableW(wname.c_str(), &value[0], n);
    value.resize(written);
    return narrow(value);
}

bool readTextFile(const std::string& path, std::string& out) {
    out.clear();
    std::FILE* f = openFile(path, L"rb");
    if (!f) {
        return false;
    }
    char chunk[4096];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
        out.append(chunk, n);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

} // namespace app
