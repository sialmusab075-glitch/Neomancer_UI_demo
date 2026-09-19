#pragma once

#include <string>

namespace app {

// Reads the current back buffer (after the HUD is drawn, before swap) and
// writes it as a 24-bit BMP. Used by the SOLSIM_SCREENSHOT dev hook for
// visual checks without a person at the screen.
bool saveBackBufferBmp(const std::string& pathUtf8, int width, int height);

} // namespace app
