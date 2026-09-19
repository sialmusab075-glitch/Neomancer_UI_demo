#include "hud/Panels.h"
#include "hud/Theme.h"
#include "sim/Constants.h"
#include "sim/SimClock.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace hud {

unsigned hexId(const char* text) {
    // FNV-1a folded to 16 bits: stable across runs and platforms.
    unsigned h = 2166136261u;
    for (const char* p = text; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 16777619u;
    }
    return (h ^ (h >> 16)) & 0xFFFFu;
}

unsigned hexId(double value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6f", value);
    return hexId(buf);
}

void formatDate(char* buf, std::size_t size, double t_days) {
    const sim::CalendarDate d = sim::calendarFromJulianDate(sim::kJ2000_JD + t_days);
    std::snprintf(buf, size, "%04d-%02d-%02d %02d:%02d", d.year, d.month, d.day, d.hour, d.minute);
}

void drawTitleBar(const sim::SimClock& clock, const HudState& state) {
    const HudTheme& t = theme();
    if (BeginPanel(kWinStatus, "SOL SYSTEM SIM", "OBS \xC2\xB7 01")) {
        const float s = dpi();

        // T+ clock: hero colour in themes with a hero glow, value colour otherwise.
        pushLargeFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.heroGlow ? t.textHero : t.textValue));
        ImGui::TextUnformatted(sim::formatElapsed(clock.elapsedDays()).c_str());
        ImGui::PopStyleColor();
        popFont();

        char date[32];
        formatDate(date, sizeof date, clock.timeDays());
        pushFontSize(t.sizeSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.data));
        ImGui::TextUnformatted(date);
        ImGui::PopStyleColor();
        popFont();
        ImGui::SameLine();
        TinyText("TT \xC2\xB7 EPOCH J2000.0");

        char buf[48];
        std::snprintf(buf, sizeof buf, "%+.2f D/S", clock.scale());
        Chip(buf, clock.scale() < 0.0 ? ChipTone::Alarm : ChipTone::Accent);
        ImGui::SameLine();
        std::snprintf(buf, sizeof buf, "%.0f FPS", static_cast<double>(state.fps));
        Chip(buf, ChipTone::Muted);
        ImGui::SameLine();
        Chip("ECL J2000", ChipTone::Muted);

        ImGui::Dummy(ImVec2(0.0f, 1.0f * s));
        StatusDot(clock.paused() ? "SIM HOLD" : "SIM RUN", clock.paused() ? t.alarm : t.accent);
        ImGui::SameLine(0.0f, 10.0f * s);
        const bool renderOk = state.fps >= 45.0f;
        StatusDot(renderOk ? "RENDER OK" : "RENDER SLOW", renderOk ? t.accent : t.alarm);
        ImGui::SameLine(0.0f, 10.0f * s);
        StatusDot(state.trueScale ? "SCALE TRUE" : "SCALE LOG", state.trueScale ? t.data : t.accent);
    }
    EndPanel();
}

} // namespace hud
