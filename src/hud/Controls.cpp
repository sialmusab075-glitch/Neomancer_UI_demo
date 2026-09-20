#include "hud/Panels.h"
#include "hud/Theme.h"
#include "sim/BodyTable.h"
#include "sim/SimClock.h"
#include "sim/SolarSystem.h"

#include <imgui.h>

#include <cstdio>

namespace hud {

namespace {

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
