// SOL SYSTEM SIM: real-time Solar System simulator with a telemetry HUD.
//
// Release builds link with /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup, so this
// plain main() is the entry point in every configuration.

#include "app/Application.h"
#include "app/Log.h"

#include <exception>
#include <string>

int main() {
    try {
        app::Application application;
        return application.run();
    } catch (const std::exception& e) {
        app::showFatalError(std::string("Unhandled exception: ") + e.what());
    } catch (...) {
        app::showFatalError("Unhandled unknown exception.");
    }
    return 1;
}
