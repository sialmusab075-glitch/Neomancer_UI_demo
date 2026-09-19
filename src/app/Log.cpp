#include "app/Log.h"

#include "app/Paths.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace app {

namespace {

FILE* g_logFile = nullptr;

void writeLine(const char* level, const char* fmt, va_list args) {
    char message[2048];
    std::vsnprintf(message, sizeof message, fmt, args);

    char line[2100];
    std::snprintf(line, sizeof line, "[%s] %s\n", level, message);

    std::fputs(line, stderr);
    OutputDebugStringA(line);
    if (g_logFile) {
        std::fputs(line, g_logFile);
        std::fflush(g_logFile);
    }
}

} // namespace

void logInit(const std::string& logFileUtf8) {
    g_logFile = openFile(logFileUtf8, L"w");
}

void logShutdown() {
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void logInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    writeLine("info", fmt, args);
    va_end(args);
}

void logError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    writeLine("error", fmt, args);
    va_end(args);
}

void showFatalError(const std::string& message) {
    logError("%s", message.c_str());
    MessageBoxW(nullptr, widen(message).c_str(), L"SOL SYSTEM SIM", MB_OK | MB_ICONERROR);
}

} // namespace app
