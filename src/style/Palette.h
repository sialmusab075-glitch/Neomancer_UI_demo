#pragma once
// The single source of colour for the whole app: 3D scene (render/GlColor.h)
// and HUD (hud/Theme.h) both read these tokens. No dependencies, so any layer
// can include it.
//
// Two accents give the scene and the HUD separate identities:
//   WORLD (warm gold)  - things in space: grid, Sun particles, scene labels,
//                        event timestamps, histogram bars.
//   UI    (teal)       - the instrument: panel chrome, buttons, and the
//                        currently SELECTED target only.
// Everything else is neutral blue-grey at low alpha, so that brightness,
// not hue, carries importance.

#include <cstdint>

namespace palette {

// 0xRRGGBB
using Rgb = std::uint32_t;

// --- surfaces --------------------------------------------------------------------
constexpr Rgb SceneBg     = 0x070B10u; // 3D view: darker and a touch warmer than panels
constexpr Rgb PanelBg     = 0x0B1520u; // panel body (drawn at PanelAlpha)
constexpr Rgb PanelTop    = 0x10202Eu; // top of the panel's vertical gradient
constexpr Rgb HeaderStrip = 0x08101Au; // darker strip behind panel headers
constexpr Rgb BlockFill   = 0x08111Au; // readout blocks, log body, plot frame
constexpr float PanelAlpha = 0.92f;

// --- accents ---------------------------------------------------------------------
constexpr Rgb World    = 0xC9A45Cu; // warm gold
constexpr Rgb WorldDim = 0x7A6A45u;
constexpr Rgb Ui       = 0x4FD1C5u; // teal
constexpr Rgb UiDim    = 0x2E7F78u;
constexpr Rgb Warn     = 0xE08A3Cu; // warnings only (hold, reversed time, out of range)
constexpr Rgb Neutral  = 0x6F8499u; // unselected orbits, planet rim light

// --- text: four brightness levels -------------------------------------------------
constexpr Rgb TextHero  = 0xE8FBF8u; // hero numbers, selected names (bright white-teal)
constexpr Rgb TextValue = 0xC3D0DAu; // values
constexpr Rgb TextLabel = 0x5E7282u; // labels
constexpr Rgb TextMicro = 0x3A4957u; // hex codes, tick values, captions

// --- stars (cool, never the UI teal) -----------------------------------------------
constexpr Rgb StarWhite = 0xD2DEFFu;
constexpr Rgb StarWarm  = 0xFFC888u;
constexpr Rgb StarCool  = 0x9FC8EEu;

constexpr float red(Rgb c) { return static_cast<float>((c >> 16) & 0xFFu) / 255.0f; }
constexpr float green(Rgb c) { return static_cast<float>((c >> 8) & 0xFFu) / 255.0f; }
constexpr float blue(Rgb c) { return static_cast<float>(c & 0xFFu) / 255.0f; }

} // namespace palette
