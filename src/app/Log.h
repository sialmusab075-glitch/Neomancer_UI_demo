#pragma once

#include <string>

namespace app {

// Minimal logger. Writes to stderr (visible in Debug, which keeps the console),
// the debugger output window, and a log file next to the executable (useful
// for Release builds, which have no console).
void logInit(const std::string& logFileUtf8);
void logShutdown();
void logInfo(const char* fmt, ...);
void logError(const char* fmt, ...);

// Logs the message and shows a modal error box. Used for unrecoverable
// startup failures (missing assets, no OpenGL 3.3).
void showFatalError(const std::string& message);

} // namespace app
