#pragma once
// HUD style and widget kit. All colours, sizes and fonts come from theme()
// (hud/HudTheme.h); widgets branch on the theme's component switches where the
// two looks differ structurally.

#include "hud/HudTheme.h"

#include <imgui.h>

namespace hud {

// ImGuiStyle for the given theme (unscaled; the app applies DPI on top).
void applyTheme(ImGuiStyle& style, const HudTheme& t);

// Fonts.
ImFont* regularFont();
ImFont* largeFont();
void pushLargeFont();                 // large font at its native 28 px
void pushFontSize(float unscaledPx);  // regular font at another size
void popFont();

// Current DPI factor (style.FontScaleDpi).
float dpi();

// ---------------------------------------------------------------------------
// Widgets. Sizes are unscaled unless noted; each advances the layout cursor.
// ---------------------------------------------------------------------------

enum class ChipTone {
    Accent, // status tag; inverted (filled) in themes with invertedChips
    Alarm,  // attention; always filled
    Data,   // data-coloured outline
    Muted,  // quiet outline in the label colour
    Micro,  // quietest outline (IDs, hex codes)
};

// Small tag, e.g. [OBS · 01]. Inline: use ImGui::SameLine() between chips.
// `filled` forces the filled look regardless of the theme.
void Chip(const char* text, ChipTone tone = ChipTone::Accent, bool filled = false);
// The same chip drawn at an explicit position (no layout); returns its size in pixels.
ImVec2 ChipAt(ImDrawList* dl, ImVec2 pos, const char* text, ChipTone tone, bool filled = false);
ImVec2 ChipSize(const char* text);
// Selector chip: filled when active, outline otherwise. Returns true when clicked.
bool ChipToggle(const char* text, bool active);

// Themed button: flat outline (hover = accent border) or the original filled
// style; `active` renders it as a selected (inverted) chip. width in pixels (0 = auto).
bool Button(const char* label, float widthPx = 0.0f, bool active = false);
// Small button in the label size.
bool SmallButton(const char* label);
// Themed checkbox: a small square filled with the accent when on.
bool Checkbox(const char* label, bool* v);

// Readout tile: tiny label, value, thin meter with min/max text at the ends.
// `fraction` is clamped to [0, 1]; out-of-range values draw in the alarm colour.
// width in pixels; <= 0 takes the available width.
void ReadoutBar(const char* label, const char* value, float fraction, const char* minText, const char* maxText,
                float width = 0.0f, bool outOfRange = false);

// Circular gauge (OBSERVATORY). diameter unscaled.
void RingGauge(const char* label, float fraction, const char* centreText, float diameter, ImU32 col);

// Tall vertical gauge (REACTOR): value above, gradient bar (dark at the bottom,
// bright at the top), label below. Sizes in pixels. `highlight` marks the
// selected item.
void BarGauge(const char* label, const char* valueText, float fraction, float widthPx, float heightPx,
              bool highlight = false);

// Thin horizontal meter in a rectangle (pixels): track in the line colour, fill in `fill`.
void Meter(ImDrawList* dl, ImVec2 min, ImVec2 max, float fraction, ImU32 fill);

// Four L-shaped corner marks around a rectangle (screen coordinates).
void CornerBrackets(ImDrawList* dl, ImVec2 min, ImVec2 max, float len, ImU32 col, float thickness = 1.0f);

// Dashed rectangle outline (screen coordinates, pixel dash/gap lengths).
void DashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float dash, float gap, float thickness);

// Hero readout: `bigDecimals` decimals in large type, then `supDigits` more as a
// small raised suffix, plus a unit caption. E.g. (0.98831, 2, 2) -> "0.98" + "83".
void HeroNumber(double value, int bigDecimals, int supDigits, const char* unit);

// Small status pip with a label, for the title bar.
void StatusDot(const char* label, ImU32 col);

// Text in the label size (default colour: label).
void TinyText(const char* text);
void TinyText(const char* text, ImU32 col);
// Text in the micro size and colour.
void MicroText(const char* text);

// Title text (tracked font where the theme uses tracking) at an unscaled size.
void TitleText(ImDrawList* dl, ImVec2 pos, float sizeUnscaled, ImU32 col, const char* text);
float TitleTextWidth(float sizeUnscaled, const char* text);

// HUD panel: an ImGui window with the theme's chrome (header strip, tracked
// title, tag chip, corner ticks, optional glow/gradient, scanlines).
// Always pair with EndPanel(). Returns false when collapsed/hidden.
bool BeginPanel(const char* windowName, const char* title, const char* tag, ImGuiWindowFlags flags = 0);
void EndPanel();

// Scanline texture over a rectangle (only where the theme enables it).
void Scanlines(ImDrawList* dl, ImVec2 min, ImVec2 max);

} // namespace hud
