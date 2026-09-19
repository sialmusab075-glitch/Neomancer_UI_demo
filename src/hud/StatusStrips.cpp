#include "hud/LogPanel.h"
#include "hud/Panels.h"
#include "hud/Theme.h"
#include "sim/SimClock.h"

#include <imgui.h>
#include <imgui_internal.h> // BeginViewportSideBar

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace hud {

namespace {

constexpr ImGuiWindowFlags kStripFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoFocusOnAppearing;

float stripHeight() { return std::round((theme().sizeLabel + 10.0f) * dpi()); }

// Begins a strip window along one edge of the main viewport, styled from the theme.
bool beginStrip(const char* name, ImGuiDir dir) {
    const HudTheme& t = theme();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(t.bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::BeginViewportSideBar(name, ImGui::GetMainViewport(), dir, stripHeight(), kStripFlags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return open;
}

void localClock(char* buf, std::size_t size) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::snprintf(buf, size, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Draws label/value micro pairs left to right; returns the x after the last one.
float microPair(ImDrawList* dl, float x, float y, const char* label, const char* value) {
    const HudTheme& t = theme();
    const float s = dpi();
    ImFont* f = regularFont();
    const float px = t.sizeMicro * s;
    dl->AddText(f, px, ImVec2(x, y), t.textLabel, label);
    x += f->CalcTextSizeA(px, 1e9f, 0.0f, label).x + 4.0f * s;
    dl->AddText(f, px, ImVec2(x, y), t.textValue, value);
    return x + f->CalcTextSizeA(px, 1e9f, 0.0f, value).x + 14.0f * s;
}

// Border along the strip edge facing the dock area, with accent ticks at both ends.
void stripEdge(ImDrawList* dl, ImVec2 wp, ImVec2 we, bool bottomEdge) {
    const HudTheme& t = theme();
    const float s = dpi();
    const float y = bottomEdge ? we.y - 1.0f : wp.y;
    dl->AddLine(ImVec2(wp.x, y), ImVec2(we.x, y), t.line, 1.0f);
    dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + 24.0f * s, y), t.accent, 1.0f);
    dl->AddLine(ImVec2(we.x - 24.0f * s, y), ImVec2(we.x, y), t.accent, 1.0f);
}

} // namespace

void drawTopStrip(const sim::SimClock& clock, const HudState& state, unsigned sessionId) {
    const HudTheme& t = theme();
    if (!t.statusStrips) {
        return;
    }
    if (beginStrip("##sol_topstrip", ImGuiDir_Up)) {
        const float s = dpi();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const ImVec2 we(wp.x + ws.x, wp.y + ws.y);
        const float mic = t.sizeMicro * s;
        const float yText = wp.y + 0.5f * (ws.y - mic);

        // Left: marker, tracked name, session / epoch / date / elapsed.
        float x = wp.x + 10.0f * s;
        dl->AddRectFilled(ImVec2(x, wp.y + 0.5f * ws.y - 3.0f * s), ImVec2(x + 6.0f * s, wp.y + 0.5f * ws.y + 3.0f * s),
                          t.accent);
        x += 12.0f * s;
        const float titlePx = t.sizeLabel;
        TitleText(dl, ImVec2(x, wp.y + 0.5f * (ws.y - titlePx * s)), titlePx, t.textHero, "SOL SYSTEM SIM");
        x += TitleTextWidth(titlePx, "SOL SYSTEM SIM") + 18.0f * s;

        char buf[48];
        std::snprintf(buf, sizeof buf, "0x%04X", sessionId & 0xFFFFu);
        x = microPair(dl, x, yText, "SES", buf);
        x = microPair(dl, x, yText, "EPOCH", "J2000.0");
        formatDate(buf, sizeof buf, clock.timeDays());
        x = microPair(dl, x, yText, "SIM DATE", buf);
        x = microPair(dl, x, yText, "ELAPSED", sim::formatElapsed(clock.elapsedDays()).c_str());
        std::snprintf(buf, sizeof buf, "%+.2f D/S", clock.scale());
        microPair(dl, x, yText, "RATE", buf);

        // Right: FPS, local time, and the run/hold chip.
        const char* chip = clock.paused() ? "HOLD" : "SIM RUN";
        const ImVec2 cs = ChipSize(chip);
        float rx = we.x - 10.0f * s - cs.x;
        ChipAt(dl, ImVec2(rx, wp.y + 0.5f * (ws.y - cs.y)), chip, clock.paused() ? ChipTone::Alarm : ChipTone::Accent,
               true);
        ImFont* f = regularFont();
        char clockBuf[16];
        localClock(clockBuf, sizeof clockBuf);
        char right[64];
        std::snprintf(right, sizeof right, "FPS %.0f   LOCAL %s", static_cast<double>(state.fps), clockBuf);
        rx -= f->CalcTextSizeA(mic, 1e9f, 0.0f, right).x + 12.0f * s;
        dl->AddText(f, mic, ImVec2(rx, yText), t.textValue, right);

        stripEdge(dl, wp, we, true);
        Scanlines(dl, wp, we);
    }
    ImGui::End();
}

void drawBottomStrip(const HudState& state, const EventLog& log, const char* targetName) {
    const HudTheme& t = theme();
    if (!t.statusStrips) {
        return;
    }
    if (beginStrip("##sol_bottomstrip", ImGuiDir_Down)) {
        const float s = dpi();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const ImVec2 we(wp.x + ws.x, wp.y + ws.y);

        // Left: layer/status chips (filled when on).
        float x = wp.x + 10.0f * s;
        auto chip = [&](const char* text, bool on) {
            const ImVec2 cs = ChipSize(text);
            ChipAt(dl, ImVec2(x, wp.y + 0.5f * (ws.y - cs.y)), text, on ? ChipTone::Accent : ChipTone::Muted, on);
            x += cs.x + 5.0f * s;
        };
        chip(state.showOrbits ? "ORBITS ACTIVE" : "ORBITS OFF", state.showOrbits);
        chip("GRID", state.showGrid);
        chip("STARS", state.showStars);
        chip("LABELS", state.showLabels);
        chip("BLOOM", state.bloom);
        chip("FINISH", state.finish);
        chip(state.trueScale ? "SCALE 1:1" : "SCALE LOG10", true);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s %s", state.following ? "TRACK" : "TGT", targetName);
        chip(buf, state.following);

        // Right: latest event log line in micro text, clipped to the space left.
        const float mic = t.sizeMicro * s;
        const float yText = wp.y + 0.5f * (ws.y - mic);
        if (!log.entries().empty()) {
            const EventLog::Entry& e = log.entries().back();
            ImFont* f = regularFont();
            const float x0 = x + 12.0f * s;
            const float x1 = we.x - 10.0f * s;
            dl->PushClipRect(ImVec2(x0, wp.y), ImVec2(x1, we.y), true);
            float lx = x0;
            dl->AddText(f, mic, ImVec2(lx, yText), t.textLabel, "LAST EVT");
            lx += f->CalcTextSizeA(mic, 1e9f, 0.0f, "LAST EVT").x + 6.0f * s;
            dl->AddText(f, mic, ImVec2(lx, yText), t.textLabel, e.stamp.c_str());
            lx += f->CalcTextSizeA(mic, 1e9f, 0.0f, e.stamp.c_str()).x + 6.0f * s;
            dl->AddText(f, mic, ImVec2(lx, yText), t.textValue, e.text.c_str());
            dl->PopClipRect();
        }

        stripEdge(dl, wp, we, false);
        Scanlines(dl, wp, we);
    }
    ImGui::End();
}

} // namespace hud
