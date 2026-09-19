#pragma once
// Every HUD colour, size, font and component choice, in one struct.
// Two instances exist: REACTOR (red/amber monochrome, default) and
// OBSERVATORY (the original teal look). HUD code reads theme(); nothing in
// src/hud hard-codes a colour.

#include "style/Palette.h"
#include "style/SceneStyle.h"

#include <imgui.h>

namespace hud {

enum class ThemeId { Reactor = 0, Observatory = 1 };
constexpr int kThemeCount = 2;

struct HudTheme {
    const char* name;

    // --- surfaces --------------------------------------------------------------------
    ImU32 bg;        // base behind panels; status strips
    ImU32 panel;     // panel fill (ImGuiCol_WindowBg)
    ImU32 panelTop;  // top of the panel gradient (when gradientPanels)
    ImU32 header;    // strip behind panel titles
    ImU32 block;     // tiles, log body, plot frame, table rows
    ImU32 rowAlt;    // alternating table row
    ImU32 line;      // 1 px borders, meter tracks
    ImU32 frame;     // input frames (slider, combo)

    // --- text ------------------------------------------------------------------------
    ImU32 textHero;
    ImU32 textValue;
    ImU32 textLabel;
    ImU32 textMicro;

    // --- accents ---------------------------------------------------------------------
    ImU32 accent;    // primary accent (chips, active states, meters)
    ImU32 accentDim;
    ImU32 data;      // data markers, timestamps, histogram bars
    ImU32 dataDim;
    ImU32 alarm;     // attention states only
    ImU32 cool;      // sparing cool contrast
    ImU32 chipText;  // text on filled chips
    ImU32 corner;    // panel corner ticks
    ImU32 hero;      // hero numbers

    // --- the 3D scene's look for this theme (background, rings, core, grade, labels) ---
    style::SceneStyle scene;

    // --- sizes (unscaled px; DPI applied on top) ------------------------------------------
    float sizeMicro;     // IDs, hex codes, units, tick values
    float sizeLabel;     // labels, chips, captions (>= 10.5)
    float sizeSmall;     // panel titles, log lines
    float sizeValue;     // readout values
    float sizeLarge;     // T+ clock
    float sizeHero;      // hero number
    float titleTracking; // extra advance between title glyphs (px at 100%)
    float scrollbar;
    float cornerTick;
    float meterHeight;
    float scanlineAlpha; // 0 = no scanlines
    float plotFillAlpha;

    // --- component choices ---------------------------------------------------------------
    bool invertedChips;  // status tags as filled chips with dark text
    bool dashedCallout;  // dashed box around the TARGET hero metric
    bool barGauges;      // vertical bar gauges instead of rings / flat histogram
    bool denseTable;     // STATE VECTOR as a table instead of 2x4 blocks
    bool flatControls;   // outline buttons, square checkboxes, thin scrollbars
    bool heroGlow;
    bool gradientPanels;
    bool outerGlow;      // second border 1 px outside each panel
    bool tileBrackets;   // corner ticks on readout tiles
    bool dottedPlotGrid;
    bool logMarkers;     // red square before alarm log lines, split number colouring
    bool statusStrips;   // full-width top and bottom strips

    // --- fonts (assigned after loading) --------------------------------------------------
    ImFont* fontBody;
    ImFont* fontTitle; // tracked instance for titles (== fontBody when no tracking)
    ImFont* fontLarge;
};

const HudTheme& theme();
const HudTheme& themeById(ThemeId id);
ThemeId themeId();
void setTheme(ThemeId id);
const char* themeName(ThemeId id);

// Fonts are shared by both themes; titles use the tracked instance only where
// the theme asks for tracking.
void setThemeFonts(ImFont* body, ImFont* large, ImFont* trackedTitle);

// Helper for alpha variants of a token.
ImU32 withAlpha(ImU32 c, float alphaMultiplier);

// A 0xRRGGBB scene token as an ImGui colour.
constexpr ImU32 toU32(palette::Rgb c, float a = 1.0f) {
    return IM_COL32((c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu, static_cast<unsigned>(a * 255.0f + 0.5f));
}

} // namespace hud
