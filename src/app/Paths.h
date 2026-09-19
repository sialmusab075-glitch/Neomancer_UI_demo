#pragma once

#include <cstdio>
#include <string>

namespace app {

// All strings are UTF-8. Paths are resolved relative to the executable, never
// the working directory, so the app behaves the same when launched from
// Visual Studio, a terminal, or Explorer.

// Directory containing the running executable, without a trailing separator.
std::string executableDir();

// executableDir() + "/assets/" + relative.
std::string assetPath(const std::string& relative);

bool fileExists(const std::string& path);

// Reads a whole file. Returns false (and leaves `out` empty) on failure.
bool readTextFile(const std::string& path, std::string& out);

// Opens a file by UTF-8 path (wide-char mode, e.g. L"rb"). Returns nullptr on
// failure. Works with both MSVC and MinGW-w64.
std::FILE* openFile(const std::string& pathUtf8, const wchar_t* mode);

// Value of an environment variable (UTF-8), or an empty string if unset.
std::string envVar(const char* name);

// UTF-8 <-> UTF-16 for Win32 APIs.
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);

} // namespace app
