#include "hud/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace hud {

namespace {

bool g_panelVisible = false;

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImVec4 colA(ImU32 c, float a) {
    ImVec4 v = col(c);
    v.w *= a;
    return v;
}

ImFont* regular() {
    ImFont* f = theme().fontBody;
    return f ? f : ImGui::GetFont();
}

ImVec2 textSize(ImFont* f, float px, const char* text) { return f->CalcTextSizeA(px, 1e9f, 0.0f, text); }

// Colours of a chip tone: {fill, border, text, filled?}.
struct ChipLook {
    ImU32 fill, border, text;
    bool filled;
};

ChipLook chipLook(ChipTone tone, bool forceFilled) {
    const HudTheme& t = theme();
    ImU32 c = t.accent;
    switch (tone) {
    case ChipTone::Accent: c = t.accent; break;
    case ChipTone::Alarm: c = t.alarm; break;
    case ChipTone::Data: c = t.data; break;
    case ChipTone::Muted: c = t.textLabel; break;
    case ChipTone::Micro: c = t.textMicro; break;
    }
    const bool filled = forceFilled || (t.invertedChips && (tone == ChipTone::Accent || tone == ChipTone::Alarm));
    if (filled) {
        return {withAlpha(c, t.invertedChips ? 1.0f : 0.85f), c, t.chipText, true};
    }
    const ImU32 border = t.invertedChips ? t.line : withAlpha(c, 0.65f);
    return {withAlpha(c, t.invertedChips ? 0.0f : 0.07f), border, c, false};
}

// Draws a chip at `p`. Returns its size.
ImVec2 drawChipAt(ImDrawList* dl, ImVec2 p, const char* text, const ChipLook& look) {
    const HudTheme& t = theme();
    const float s = dpi();
    const float px = t.sizeLabel * s;
    ImFont* f = regular();
    const ImVec2 ts = textSize(f, px, text);
    const ImVec2 pad(4.0f * s, 1.5f * s);
    const ImVec2 sz(ts.x + 2.0f * pad.x, ts.y + 2.0f * pad.y);
    const ImVec2 q(p.x + sz.x, p.y + sz.y);
    if (look.fill & IM_COL32_A_MASK) {
        dl->AddRectFilled(p, q, look.fill);
    }
    if (!look.filled) {
        dl->AddRect(p, q, look.border, 0.0f, 0, 1.0f);
    }
    dl->AddText(f, px, ImVec2(p.x + pad.x, p.y + pad.y), look.text, text);
    return sz;
}

ImVec2 drawChip(const char* text, const ChipLook& look) {
    return drawChipAt(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), text, look);
}

} // namespace

void applyTheme(ImGuiStyle& style, const HudTheme& t) {
    ImGui::StyleColorsDark(&style);

    // Square, thin, flat.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 3.0f);
    style.ItemSpacing = ImVec2(6.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.ScrollbarSize = t.scrollbar;
    style.GrabMinSize = t.flatControls ? 6.0f : 8.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.DockingSeparatorSize = 2.0f;
    style.TabBarOverlineSize = 1.0f;
    style.CellPadding = ImVec2(4.0f, 1.0f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = col(t.textValue);
    c[ImGuiCol_TextDisabled]         = col(t.textLabel);
    c[ImGuiCol_WindowBg]             = col(t.panel);
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = colA(t.panel, 1.03f);
    c[ImGuiCol_Border]               = col(t.line);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = col(t.frame);
    c[ImGuiCol_FrameBgHovered]       = colA(t.accent, 0.12f);
    c[ImGuiCol_FrameBgActive]        = colA(t.accent, 0.22f);
    c[ImGuiCol_TitleBg]              = col(t.header);
    c[ImGuiCol_TitleBgActive]        = col(t.panelTop);
    c[ImGuiCol_TitleBgCollapsed]     = colA(t.header, 0.70f);
    c[ImGuiCol_MenuBarBg]            = col(t.header);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = t.flatControls ? col(t.line) : colA(t.accent, 0.20f);
    c[ImGuiCol_ScrollbarGrabHovered] = t.flatControls ? col(t.accent) : colA(t.accent, 0.40f);
    c[ImGuiCol_ScrollbarGrabActive]  = t.flatControls ? col(t.accent) : colA(t.accent, 0.60f);
    c[ImGuiCol_CheckMark]            = col(t.accent);
    c[ImGuiCol_SliderGrab]           = colA(t.accent, 0.80f);
    c[ImGuiCol_SliderGrabActive]     = col(t.accent);
    c[ImGuiCol_Button]               = t.flatControls ? ImVec4(0, 0, 0, 0) : colA(t.accent, 0.06f);
    c[ImGuiCol_ButtonHovered]        = colA(t.accent, t.flatControls ? 0.08f : 0.20f);
    c[ImGuiCol_ButtonActive]         = colA(t.accent, t.flatControls ? 0.25f : 0.36f);
    c[ImGuiCol_Header]               = colA(t.accent, 0.12f);
    c[ImGuiCol_HeaderHovered]        = colA(t.accent, 0.22f);
    c[ImGuiCol_HeaderActive]         = colA(t.accent, 0.32f);
    c[ImGuiCol_Separator]            = t.flatControls ? col(t.line) : colA(t.accent, 0.18f);
    c[ImGuiCol_SeparatorHovered]     = colA(t.accent, 0.55f);
    c[ImGuiCol_SeparatorActive]      = col(t.accent);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = colA(t.accent, 0.45f);
    c[ImGuiCol_ResizeGripActive]     = col(t.accent);
    c[ImGuiCol_Tab]                  = col(t.header);
    c[ImGuiCol_TabHovered]           = colA(t.accent, 0.30f);
    c[ImGuiCol_TabSelected]          = col(t.panelTop);
    c[ImGuiCol_TabSelectedOverline]  = col(t.accent);
    c[ImGuiCol_TabDimmed]            = colA(t.header, 0.80f);
    c[ImGuiCol_TabDimmedSelected]    = col(t.panel);
    c[ImGuiCol_DockingPreview]       = colA(t.accent, 0.35f);
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PlotLines]            = col(t.accent);
    c[ImGuiCol_PlotHistogram]        = col(t.data);
    c[ImGuiCol_TableHeaderBg]        = col(t.header);
    c[ImGuiCol_TableBorderStrong]    = col(t.line);
    c[ImGuiCol_TableBorderLight]     = colA(t.line, 0.6f);
    c[ImGuiCol_TableRowBg]           = col(t.block);
    c[ImGuiCol_TableRowBgAlt]        = col(t.rowAlt);
    c[ImGuiCol_TextSelectedBg]       = colA(t.accent, 0.30f);
    c[ImGuiCol_NavCursor]            = col(t.accent);
}

ImFont* regularFont() { return regular(); }
ImFont* largeFont() { return theme().fontLarge ? theme().fontLarge : regular(); }

void pushLargeFont() {
    // ImGui 1.92: PushFont takes an explicit size; LegacySize is the size the
    // font was added with (28 px). Global DPI scaling is applied on top.
    ImFont* f = largeFont();
    ImGui::PushFont(f, f->LegacySize);
}

void pushFontSize(float unscaledPx) { ImGui::PushFont(regular(), unscaledPx); }

void popFont() { ImGui::PopFont(); }

float dpi() { return ImGui::GetStyle().FontScaleDpi; }

// --- chips / buttons / checkboxes -------------------------------------------------------

void Chip(const char* text, ChipTone tone, bool filled) { ImGui::Dummy(drawChip(text, chipLook(tone, filled))); }

ImVec2 ChipAt(ImDrawList* dl, ImVec2 pos, const char* text, ChipTone tone, bool filled) {
    return drawChipAt(dl, pos, text, chipLook(tone, filled));
}

ImVec2 ChipSize(const char* text) {
    const float s = dpi();
    const ImVec2 ts = textSize(regular(), theme().sizeLabel * s, text);
    return ImVec2(ts.x + 8.0f * s, ts.y + 3.0f * s);
}

bool ChipToggle(const char* text, bool active) {
    const ChipLook look = active ? chipLook(ChipTone::Accent, true) : chipLook(ChipTone::Muted, false);
    const ImVec2 sz = drawChip(text, look);
    ImGui::PushID(text);
    const bool clicked = ImGui::InvisibleButton("##chip", sz);
    ImGui::PopID();
    if (ImGui::IsItemHovered() && !active) {
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme().accent, 0.0f,
                                            0, 1.0f);
    }
    return clicked;
}

bool Button(const char* label, float widthPx, bool active) {
    const HudTheme& t = theme();
    int pushed = 0;
    if (active) {
        // Selected: inverted chip style (filled accent, dark text) in both themes'
        // own flavour (the original look used a 32% accent fill + bright text).
        ImGui::PushStyleColor(ImGuiCol_Button, t.flatControls ? col(t.accent) : colA(t.accent, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.flatControls ? col(t.accent) : colA(t.accent, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_Text, col(t.flatControls ? t.chipText : t.textHero));
        pushed = 3;
    }
    const bool pressed = ImGui::Button(label, ImVec2(widthPx, 0.0f));
    ImGui::PopStyleColor(pushed);
    if (t.flatControls && !active && ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), t.accent, 0.0f, 0,
                                            1.0f);
    }
    return pressed;
}

bool SmallButton(const char* label) {
    pushFontSize(theme().sizeLabel);
    const bool pressed = ImGui::SmallButton(label);
    if (theme().flatControls && ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme().accent, 0.0f,
                                            0, 1.0f);
    }
    popFont();
    return pressed;
}

bool Checkbox(const char* label, bool* v) {
    const HudTheme& t = theme();
    if (!t.flatControls) {
        return ImGui::Checkbox(label, v);
    }
    // Small square, filled with the accent when on; the label is part of the hit area.
    const float s = dpi();
    const float box = std::round(9.0f * s);
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const float h = std::max(ts.y, box);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##cb", ImVec2(box + 6.0f * s + ts.x, h));
    ImGui::PopID();
    if (clicked) {
        *v = !*v;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b0(p.x, p.y + 0.5f * (h - box));
    const ImVec2 b1(b0.x + box, b0.y + box);
    const float a = ImGui::GetStyle().Alpha; // lowered inside BeginDisabled()
    dl->AddRect(b0, b1, withAlpha(hovered ? t.accent : t.line, a), 0.0f, 0, 1.0f);
    if (*v) {
        const float in = std::max(1.0f, std::round(2.0f * s));
        dl->AddRectFilled(ImVec2(b0.x + in, b0.y + in), ImVec2(b1.x - in, b1.y - in), withAlpha(t.accent, a));
    }
    // Label up to "##".
    const char* end = std::strstr(label, "##");
    dl->AddText(ImVec2(b1.x + 6.0f * s, p.y + 0.5f * (h - ts.y)), withAlpha(*v ? t.textValue : t.textLabel, a),
                label, end);
    return clicked;
}

// --- tiles, gauges, meters -----------------------------------------------------------------

void Meter(ImDrawList* dl, ImVec2 min, ImVec2 max, float fraction, ImU32 fill) {
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    dl->AddRectFilled(min, max, theme().line);
    dl->AddRectFilled(min, ImVec2(min.x + (max.x - min.x) * f, max.y), fill);
}

void ReadoutBar(const char* label, const char* value, float fraction, const char* minText, const char* maxText,
                float width, bool outOfRange) {
    const HudTheme& t = theme();
    const float s = dpi();
    if (width <= 0.0f) {
        width = ImGui::GetContentRegionAvail().x;
    }
    ImFont* f = regular();
    const float lab = t.sizeLabel * s;
    const float mic = t.sizeMicro * s;
    const float val = t.sizeValue * s;
    const float pad = (t.flatControls ? 4.0f : 5.0f) * s;
    const float h = pad + lab + 2.0f * s + val + 3.0f * s + std::max(lab, mic) + pad;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + width, p.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = outOfRange ? t.alarm : t.accent;

    dl->AddRectFilled(p, q, t.block);
    dl->AddRect(p, q, t.line, 0.0f, 0, 1.0f);
    if (t.tileBrackets) {
        CornerBrackets(dl, p, q, 4.0f * s, withAlpha(t.accent, 0.60f), 1.0f);
    }

    float y = p.y + pad;
    const float x0 = p.x + pad;
    const float x1 = q.x - pad;
    dl->AddText(f, lab, ImVec2(x0, y), t.textLabel, label);
    y += lab + 2.0f * s;
    dl->AddText(f, val, ImVec2(x0, y), outOfRange ? t.alarm : t.textValue, value);
    y += val + 3.0f * s;

    // Bottom row: min text | meter | max text.
    const float minW = textSize(f, mic, minText).x;
    const float maxW = textSize(f, mic, maxText).x;
    dl->AddText(f, mic, ImVec2(x0, y), t.textMicro, minText);
    dl->AddText(f, mic, ImVec2(x1 - maxW, y), t.textMicro, maxText);
    const float bx0 = x0 + minW + 5.0f * s;
    const float bx1 = std::max(bx0 + 4.0f * s, x1 - maxW - 5.0f * s);
    const float barH = std::max(1.0f, t.meterHeight * s);
    const float by = y + 0.5f * (mic - barH);
    Meter(dl, ImVec2(bx0, by), ImVec2(bx1, by + barH), fraction, fill);

    ImGui::Dummy(ImVec2(width, h));
}

void RingGauge(const char* label, float fraction, const char* centreText, float diameter, ImU32 c) {
    const HudTheme& t = theme();
    const float s = dpi();
    ImFont* f = regular();
    const float d = diameter * s;
    const float r = 0.5f * d - 2.0f * s;
    const float thick = 3.0f * s;
    const float lab = t.sizeLabel * s;
    const float cellW = std::max(d, textSize(f, lab, label).x);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 centre(p.x + 0.5f * cellW, p.y + 0.5f * d);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float pi = 3.14159265f;
    dl->AddCircle(centre, r, withAlpha(c, 0.14f), 48, thick);
    const float frac = std::clamp(fraction, 0.0f, 1.0f);
    if (frac > 0.0f) {
        const float a0 = -0.5f * pi;
        dl->PathArcTo(centre, r, a0, a0 + 2.0f * pi * frac, 48);
        dl->PathStroke(c, 0, thick);
    }
    for (int k = 0; k < 8; ++k) {
        const float a = static_cast<float>(k) * 0.25f * pi;
        const ImVec2 dir(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(centre.x + dir.x * (r + 2.5f * s), centre.y + dir.y * (r + 2.5f * s)),
                    ImVec2(centre.x + dir.x * (r + 4.5f * s), centre.y + dir.y * (r + 4.5f * s)), t.textMicro, 1.0f);
    }
    const float small = t.sizeSmall * s;
    const ImVec2 cs = textSize(f, small, centreText);
    dl->AddText(f, small, ImVec2(centre.x - 0.5f * cs.x, centre.y - 0.5f * cs.y), t.textValue, centreText);

    const ImVec2 ls = textSize(f, lab, label);
    dl->AddText(f, lab, ImVec2(centre.x - 0.5f * ls.x, p.y + d + 3.0f * s), t.textLabel, label);
    ImGui::Dummy(ImVec2(cellW, d + 3.0f * s + lab));
}

void BarGauge(const char* label, const char* valueText, float fraction, float widthPx, float heightPx,
              bool highlight) {
    const HudTheme& t = theme();
    const float s = dpi();
    ImFont* f = regular();
    const float mic = t.sizeMicro * s;
    const float lab = t.sizeLabel * s;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Value above.
    const ImVec2 vs = textSize(f, mic, valueText);
    dl->AddText(f, mic, ImVec2(p.x + 0.5f * (widthPx - vs.x), p.y), highlight ? t.textHero : t.textValue, valueText);

    // Bar: track, then a vertical gradient fill (dark at the bottom -> bright at the top).
    const float top = p.y + mic + 3.0f * s;
    const float bottom = p.y + heightPx - lab - 3.0f * s;
    const float inset = std::max(1.0f, std::round(2.0f * s));
    const ImVec2 t0(p.x, top), t1(p.x + widthPx, bottom);
    dl->AddRectFilled(t0, t1, t.block);
    dl->AddRect(t0, t1, highlight ? t.accent : t.line, 0.0f, 0, 1.0f);
    const float frac = std::clamp(fraction, 0.0f, 1.0f);
    if (frac > 0.0f) {
        const float fy = bottom - inset - (bottom - top - 2.0f * inset) * frac;
        const ImU32 hot = highlight ? t.textHero : t.textValue;
        dl->AddRectFilledMultiColor(ImVec2(t0.x + inset, fy), ImVec2(t1.x - inset, bottom - inset), hot, hot,
                                    t.accentDim, t.accentDim);
        dl->AddLine(ImVec2(t0.x + inset, fy), ImVec2(t1.x - inset, fy), t.textHero, std::max(1.0f, 1.0f * s));
    }
    // Scale ticks every 25% on the left edge.
    for (int k = 1; k < 4; ++k) {
        const float ty = bottom - (bottom - top) * 0.25f * static_cast<float>(k);
        dl->AddLine(ImVec2(t0.x - 3.0f * s, ty), ImVec2(t0.x, ty), t.textMicro, 1.0f);
    }

    // Label below.
    const ImVec2 ls = textSize(f, lab, label);
    dl->AddText(f, lab, ImVec2(p.x + 0.5f * (widthPx - ls.x), bottom + 3.0f * s), highlight ? t.accent : t.textLabel,
                label);
    ImGui::Dummy(ImVec2(widthPx, heightPx));
}

void CornerBrackets(ImDrawList* dl, ImVec2 min, ImVec2 max, float len, ImU32 c, float thickness) {
    const float l = std::min(len, 0.5f * std::min(max.x - min.x, max.y - min.y));
    dl->AddLine(min, ImVec2(min.x + l, min.y), c, thickness);
    dl->AddLine(min, ImVec2(min.x, min.y + l), c, thickness);
    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x - l, min.y), c, thickness);
    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, min.y + l), c, thickness);
    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x + l, max.y), c, thickness);
    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x, max.y - l), c, thickness);
    dl->AddLine(max, ImVec2(max.x - l, max.y), c, thickness);
    dl->AddLine(max, ImVec2(max.x, max.y - l), c, thickness);
}

void DashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 c, float dash, float gap, float thickness) {
    const float period = std::max(1.0f, dash + gap);
    auto hline = [&](float y) {
        for (float x = min.x; x < max.x; x += period) {
            dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + dash, max.x), y), c, thickness);
        }
    };
    auto vline = [&](float x) {
        for (float y = min.y; y < max.y; y += period) {
            dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + dash, max.y)), c, thickness);
        }
    };
    hline(min.y);
    hline(max.y);
    vline(min.x);
    vline(max.x);
}

void HeroNumber(double value, int bigDecimals, int supDigits, const char* unit) {
    const HudTheme& t = theme();
    const float s = dpi();
    ImFont* big = largeFont();
    ImFont* f = regular();

    // Format once with every digit, then split: e.g. 0.98831 -> "0.98" + "83".
    // Rounding happens once, so the parts always agree.
    char full[64];
    std::snprintf(full, sizeof full, "%.*f", bigDecimals + supDigits, value);
    const char* dot = std::strchr(full, '.');
    std::size_t bigLen = std::strlen(full);
    if (dot && supDigits > 0) {
        bigLen = static_cast<std::size_t>(dot - full) + (bigDecimals > 0 ? 1u + static_cast<std::size_t>(bigDecimals) : 0u);
    }
    char bigPart[64];
    std::snprintf(bigPart, sizeof bigPart, "%.*s", static_cast<int>(bigLen), full);
    const char* supPart = (dot && supDigits > 0) ? full + bigLen : "";

    const float heroPx = t.sizeHero * s;
    const float supPx = 20.0f * s;
    const float lab = t.sizeLabel * s;
    const ImVec2 bs = textSize(big, heroPx, bigPart);
    const ImVec2 ss = textSize(big, supPx, supPart);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (t.heroGlow) {
        // Subtle glow: offset copies at low alpha behind the digits.
        const float o = 1.5f * s;
        const ImU32 g = withAlpha(t.hero, 0.16f);
        const ImVec2 offs[8] = {{-o, 0}, {o, 0}, {0, -o}, {0, o}, {-o, -o}, {o, o}, {-o, o}, {o, -o}};
        for (const ImVec2& d : offs) {
            dl->AddText(big, heroPx, ImVec2(p.x + d.x, p.y + d.y), g, bigPart);
        }
    }
    dl->AddText(big, heroPx, p, t.hero, bigPart);
    // Superscript: top-aligned with the cap height, in the value level so it reads
    // as extra precision rather than a second number.
    const float supX = p.x + bs.x + 1.0f * s;
    const float supY = p.y + 0.14f * heroPx;
    dl->AddText(big, supPx, ImVec2(supX, supY), t.textValue, supPart);
    dl->AddText(f, lab, ImVec2(supX, supY + ss.y + 2.0f * s), t.textLabel, unit);

    ImGui::Dummy(ImVec2(bs.x + 1.0f * s + std::max(ss.x, textSize(f, lab, unit).x), bs.y));
}

void StatusDot(const char* label, ImU32 c) {
    const HudTheme& t = theme();
    const float s = dpi();
    ImFont* f = regular();
    const float lab = t.sizeLabel * s;
    const float r = 3.5f * s;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ls = textSize(f, lab, label);
    const float h = std::max(ls.y, 2.0f * r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 centre(p.x + r, p.y + 0.5f * h);
    if (t.flatControls) {
        dl->AddRectFilled(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), c); // square pip
    } else {
        dl->AddCircleFilled(centre, r + 2.0f * s, withAlpha(c, 0.18f), 16); // soft halo
        dl->AddCircleFilled(centre, r, c, 16);
    }
    dl->AddText(f, lab, ImVec2(p.x + 2.0f * r + 5.0f * s, p.y + 0.5f * (h - ls.y)), t.textValue, label);
    ImGui::Dummy(ImVec2(2.0f * r + 5.0f * s + ls.x, h));
}

void TinyText(const char* text) { TinyText(text, theme().textLabel); }

void TinyText(const char* text, ImU32 c) {
    pushFontSize(theme().sizeLabel);
    ImGui::PushStyleColor(ImGuiCol_Text, col(c));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    popFont();
}

void MicroText(const char* text) {
    pushFontSize(theme().sizeMicro);
    ImGui::PushStyleColor(ImGuiCol_Text, col(theme().textMicro));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    popFont();
}

void TitleText(ImDrawList* dl, ImVec2 pos, float sizeUnscaled, ImU32 c, const char* text) {
    ImFont* f = theme().fontTitle ? theme().fontTitle : regular();
    dl->AddText(f, sizeUnscaled * dpi(), pos, c, text);
}

float TitleTextWidth(float sizeUnscaled, const char* text) {
    ImFont* f = theme().fontTitle ? theme().fontTitle : regular();
    return textSize(f, sizeUnscaled * dpi(), text).x;
}

void Scanlines(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    const HudTheme& t = theme();
    if (t.scanlineAlpha <= 0.0f) {
        return;
    }
    const float s = dpi();
    const float period = std::max(2.0f, std::round(2.0f * s));
    const ImU32 c = withAlpha(t.textHero, t.scanlineAlpha);
    for (float y = min.y; y < max.y; y += period) {
        dl->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, y + std::max(1.0f, std::round(0.5f * period))), c);
    }
}

bool BeginPanel(const char* windowName, const char* title, const char* tag, ImGuiWindowFlags flags) {
    g_panelVisible = ImGui::Begin(windowName, nullptr, flags | ImGuiWindowFlags_NoCollapse);
    if (!g_panelVisible) {
        return false;
    }
    const HudTheme& t = theme();
    const float s = dpi();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImVec2 we(wp.x + ws.x, wp.y + ws.y);

    ImFont* f = regular();
    const float small = t.sizeSmall * s;
    const float lab = t.sizeLabel * s;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = small + 8.0f * s;
    const ImVec2 q(p.x + w, p.y + h);

    dl->PushClipRectFullScreen();
    if (t.gradientPanels) {
        // Faint vertical gradient over the panel body (lighter at the top).
        dl->AddRectFilledMultiColor(wp, we, withAlpha(t.panelTop, 0.55f), withAlpha(t.panelTop, 0.55f),
                                    withAlpha(t.panelTop, 0.0f), withAlpha(t.panelTop, 0.0f));
    }
    // Strip behind the header row, edge to edge.
    dl->AddRectFilled(ImVec2(wp.x + 1.0f, wp.y + 1.0f), ImVec2(we.x - 1.0f, q.y + 2.0f * s), t.header);
    if (t.outerGlow) {
        dl->AddRect(ImVec2(wp.x - 1.0f, wp.y - 1.0f), ImVec2(we.x + 1.0f, we.y + 1.0f), withAlpha(t.accent, 0.07f),
                    0.0f, 0, 1.0f);
    }
    // Short L-shaped ticks at the four window corners, over the dim 1 px border.
    CornerBrackets(dl, wp, ImVec2(we.x - 1.0f, we.y - 1.0f), t.cornerTick * s, t.corner, std::max(1.0f, 1.0f * s));
    dl->PopClipRect();

    // Header row: marker, (tracked) title, tag chip, underline.
    dl->AddRectFilled(ImVec2(p.x + 1.0f * s, p.y + 0.5f * h - 2.5f * s), ImVec2(p.x + 6.0f * s, p.y + 0.5f * h + 2.5f * s),
                      t.accent);
    TitleText(dl, ImVec2(p.x + 12.0f * s, p.y + 0.5f * (h - small)), t.sizeSmall, t.textHero, title);
    if (tag && *tag) {
        const ImVec2 ts = textSize(f, lab, tag);
        const ImVec2 pad(4.0f * s, 1.5f * s);
        const ImVec2 cmax(q.x - 2.0f * s, p.y + 0.5f * h + 0.5f * ts.y + pad.y);
        const ImVec2 cmin(cmax.x - ts.x - 2.0f * pad.x, cmax.y - ts.y - 2.0f * pad.y);
        if (t.invertedChips) {
            dl->AddRectFilled(cmin, cmax, t.accent);
            dl->AddText(f, lab, ImVec2(cmin.x + pad.x, cmin.y + pad.y), t.chipText, tag);
        } else {
            dl->AddRect(cmin, cmax, withAlpha(t.accent, 0.55f), 0.0f, 0, 1.0f);
            dl->AddText(f, lab, ImVec2(cmin.x + pad.x, cmin.y + pad.y), t.accent, tag);
        }
    }
    dl->AddLine(ImVec2(wp.x + 1.0f, q.y + 2.0f * s), ImVec2(we.x - 1.0f, q.y + 2.0f * s),
                t.flatControls ? t.line : withAlpha(t.accent, 0.22f), 1.0f);
    ImGui::Dummy(ImVec2(w, h + 5.0f * s));
    return true;
}

void EndPanel() {
    if (g_panelVisible) {
        // Scanline texture over the whole panel, on top of its content.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        dl->PushClipRectFullScreen();
        Scanlines(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
        dl->PopClipRect();
    }
    g_panelVisible = false;
    ImGui::End();
}

} // namespace hud
