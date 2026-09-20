#include "hud/Panels.h"
#include "hud/Theme.h"
#include "neo/sim/SwarmLegend.h"
#include "neo/sim/SwarmSelection.h"
#include "sim/BodyTable.h"
#include "sim/SimClock.h"
#include "sim/SolarSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace hud {

namespace {

// 42666 -> "42,666"
std::string withCommas(int n) {
    const std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        out += digits[i];
        const std::size_t remaining = digits.size() - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) {
            out += ',';
        }
    }
    return out;
}

void sectionLabel(const char* text) {
    ImGui::Dummy(ImVec2(0.0f, 2.0f * dpi()));
    TinyText(text);
}

} // namespace

HudEvents drawControls(sim::SimClock& clock, const sim::SolarSystem& system, HudState& state) {
    HudEvents ev;
    const HudTheme& t = theme();
    if (BeginPanel(kWinControls, "MISSION CONTROL", "CTRL \xC2\xB7 02")) {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float padX = ImGui::GetStyle().WindowPadding.x;

        // --- timeline ---------------------------------------------------------------
        sectionLabel("TIMELINE");
        pushFontSize(t.sizeSmall);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float bw = (avail - 4.0f * spacing) / 5.0f;
        if (Button(clock.paused() ? "PLAY" : "HOLD", bw, clock.paused())) {
            clock.togglePause();
            ev.pauseToggled = true;
        }
        ImGui::SameLine();
        if (Button("REV", bw, clock.scale() < 0.0)) {
            clock.reverse();
            ev.reversed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!clock.paused());
        if (Button("-1 D", bw)) {
            clock.step(-1.0);
            ev.stepped = true;
        }
        ImGui::SameLine();
        if (Button("+1 D", bw)) {
            clock.step(1.0);
            ev.stepped = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("EPOCH", bw)) {
            clock.resetToEpoch();
            ev.resetToEpoch = true;
        }

        // Log-feel slider over [-365, 365] d/s.
        char buf[64];
        float slider = static_cast<float>(sim::SimClock::scaleToSlider(clock.scale()));
        std::snprintf(buf, sizeof buf, "%+.2f D/S", clock.scale());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##timescale", &slider, -1.0f, 1.0f, buf, ImGuiSliderFlags_NoInput)) {
            clock.setScale(sim::SimClock::sliderToScale(static_cast<double>(slider)));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ev.timeScaleCommitted = true;
        }
        popFont();
        TinyText("-365  <  LOG  >  +365 D/S     [SPACE] HOLD");

        // --- display scale ---------------------------------------------------------
        sectionLabel("DISPLAY SCALE");
        pushFontSize(t.sizeSmall);
        const float hw = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        if (Button("COMPRESSED", hw, !state.trueScale) && state.trueScale) {
            state.trueScale = false;
            ev.scaleModeChanged = true;
        }
        ImGui::SameLine();
        if (Button("TRUE SCALE", hw, state.trueScale) && !state.trueScale) {
            state.trueScale = true;
            ev.scaleModeChanged = true;
        }

        // --- target --------------------------------------------------------------------
        sectionLabel("TARGET");
        const char* preview = state.selected >= 0 ? system.body(state.selected).data().name : "NONE";
        ImGui::SetNextItemWidth(hw);
        if (ImGui::BeginCombo("##target", preview)) {
            for (int i = 0; i < system.bodyCount(); ++i) {
                const bool isSel = i == state.selected;
                if (ImGui::Selectable(system.body(i).data().name, isSel) && !isSel) {
                    state.selected = i;
                    ev.selectionChanged = true;
                }
                if (isSel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (Button(state.following ? "TRACKING" : "TRACK", hw, state.following)) {
            state.following = !state.following;
            ev.followChanged = true;
        }

        // --- Earth view: offered while the Earth is the target (or already in it) ----------
        const int earthIndex = system.indexOfTableRow(sim::kEarth);
        const bool earthSelected = earthIndex >= 0 && state.selected == earthIndex;
        if (earthSelected || state.earthView) {
            ImGui::Dummy(ImVec2(0.0f, 2.0f * dpi()));
            if (Button(state.earthView ? "SOLAR VIEW  [E / ESC]" : "EARTH VIEW  [E]", ImGui::GetContentRegionAvail().x,
                       state.earthView)) {
                ev.toggleEarthView = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(state.earthView ? "Fly back out to the solar system"
                                                  : "Fly in to the Earth and see near-Earth asteroid flybys");
            }
        }

        // --- NEOS: how many of the near-Earth objects to draw, and what their colour means ------
        ImGui::Dummy(ImVec2(0.0f, 2.0f * dpi()));
        {
            ImGui::BeginDisabled(!state.neoAvailable);
            // The toggle carries the live count: "NEOS: 5,000 / 42,666" (drawn objects / objects in the database).
            char toggle[80];
            std::snprintf(toggle, sizeof toggle, "NEOS: %s##neos_toggle", state.neosLabel[0] != '\0' ? state.neosLabel : "-");
            Checkbox(toggle, &state.showNeos);

            const float rowW = ImGui::GetContentRegionAvail().x;
            const float comboW = rowW * 0.47f; // preset and legend share one row
            const float legendW = rowW - comboW - ImGui::GetStyle().ItemSpacing.x;
            const int presetIndex = std::clamp(state.neoPreset, 0, neo::kSwarmPresetCount - 1);
            auto presetText = [&](int i, char* out, std::size_t n) {
                std::snprintf(out, n, "%s  (%s)", neo::toString(static_cast<neo::SwarmPreset>(i)), withCommas(state.neoPresetSize[i]).c_str());
            };
            char preview[64];
            presetText(presetIndex, preview, sizeof preview);
            ImGui::SetNextItemWidth(comboW);
            if (ImGui::BeginCombo("##neopreset", preview)) {
                for (int i = 0; i < neo::kSwarmPresetCount; ++i) {
                    char item[64];
                    presetText(i, item, sizeof item);
                    if (ImGui::Selectable(item, i == presetIndex)) {
                        state.neoPreset = i;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How many objects to draw. The numbered presets take the LARGEST first, so each\n"
                                  "one contains the smaller ones; FILTER RESULT draws what the Earth view's NEO FILTER\n"
                                  "last query returned.");
            }

            const int legendIndex = std::clamp(state.neoLegend, 0, neo::kSwarmLegendCount - 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(legendW);
            if (ImGui::BeginCombo("##neolegend", neo::toString(static_cast<neo::SwarmLegend>(legendIndex)))) {
                for (int i = 0; i < neo::kSwarmLegendCount; ++i) {
                    if (ImGui::Selectable(neo::toString(static_cast<neo::SwarmLegend>(i)), i == legendIndex)) {
                        state.neoLegend = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();

            // The legend: the ramp the points are coloured with, its ends labelled.
            const auto legend = static_cast<neo::SwarmLegend>(legendIndex);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float barH = 6.0f * dpi();
            const float barW = rowW;
            const ImU32 cFar = toU32(t.scene.orbitPlain), cMid = toU32(t.scene.structure), cNear = toU32(t.scene.accent);
            dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + barW * 0.5f, p0.y + barH), cFar, cMid, cMid, cFar);
            dl->AddRectFilledMultiColor(ImVec2(p0.x + barW * 0.5f, p0.y), ImVec2(p0.x + barW, p0.y + barH), cMid, cNear, cNear, cMid);
            ImGui::Dummy(ImVec2(barW, barH));
            TinyText(neo::legendFarText(legend));
            ImGui::SameLine(barW - ImGui::CalcTextSize(neo::legendNearText(legend)).x * 0.9f);
            TinyText(neo::legendNearText(legend));
        }

        // --- HUD theme: label and both choices on one row ---------------------------------
        ImGui::Dummy(ImVec2(0.0f, 2.0f * dpi()));
        TinyText("HUD THEME [F2]");
        ImGui::SameLine();
        const float themeW = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        for (int i = 0; i < kThemeCount; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            const bool active = state.hudTheme == i;
            if (Button(themeName(static_cast<ThemeId>(i)), themeW, active) && !active) {
                state.hudTheme = i;
                ev.themeChanged = true;
            }
        }

        // --- layers --------------------------------------------------------------------
        sectionLabel("LAYERS");
        const float cw = (ImGui::GetContentRegionAvail().x - 3.0f * spacing) / 4.0f;
        const float col2 = cw + spacing + padX;
        const float col3 = 2.0f * (cw + spacing) + padX;
        const float col4 = 3.0f * (cw + spacing) + padX;
        Checkbox("ORBITS", &state.showOrbits);
        ImGui::SameLine(col2);
        Checkbox("GRID", &state.showGrid);
        ImGui::SameLine(col3);
        Checkbox("STARS", &state.showStars);
        ImGui::SameLine(col4);
        Checkbox("LABELS", &state.showLabels);
        ImGui::BeginDisabled(true); // moons arrive in Milestone 4
        Checkbox("MOONS", &state.showMoons);
        ImGui::EndDisabled();
        ImGui::SameLine(col2);
        Checkbox("BLOOM", &state.bloom);
        ImGui::SameLine(col3);
        Checkbox("FINISH", &state.finish);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Vignette + faint animated noise");
        }
        ImGui::SameLine(col4);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##warmth", &state.warmth, 0.0f, 1.0f, "WARM %.2f", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Scene colour grade: shadow lift, near-white highlights, red saturation clamp");
        }

        popFont();

        // --- system ----------------------------------------------------------------------
        sectionLabel("CLICK SELECT \xC2\xB7 DBL-CLICK/F TRACK \xC2\xB7 ESC RELEASE \xC2\xB7 HOME VIEW");
        if (SmallButton("RESET LAYOUT")) {
            ev.resetLayout = true;
        }
        ImGui::SameLine();
        pushFontSize(t.sizeLabel);
        Checkbox("IMGUI DEMO", &state.showImGuiDemo);
        ImGui::SameLine();
        Checkbox("IMPLOT DEMO", &state.showImPlotDemo);
        popFont();
    }
    EndPanel();
    return ev;
}

} // namespace hud
