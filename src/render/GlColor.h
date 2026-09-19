#pragma once

#include "style/Palette.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>

namespace render {

// Palette tokens (style/Palette.h) as GL-friendly vectors.
inline glm::vec3 rgb(std::uint32_t hex) { return glm::vec3(palette::red(hex), palette::green(hex), palette::blue(hex)); }

inline glm::vec4 rgba(std::uint32_t hex, float alpha) { return glm::vec4(rgb(hex), alpha); }

} // namespace render
