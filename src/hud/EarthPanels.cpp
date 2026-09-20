#include "hud/EarthUi.h"

#include "hud/Panels.h"
#include "hud/Theme.h"
#include "neo/model/JulianDate.h"
#include "neo/query/Query.h"
#include "sim/Constants.h"

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, for docking a new panel beside an existing one

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace hud {

namespace {

std::string thousands(std::size_t n) {
    std::string digits = std::to_string(n);
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

// Docks a panel into the node that already holds `sibling`, the first time it appears.
// A fresh layout (RESET LAYOUT) docks the new panels through the DockBuilder; this
// covers an imgui.ini saved before the Earth view existed, which knows nothing of them.
void dockBeside(const char* siblingWindow) {
    ImGuiID dock = 0;
    if (const ImGuiWindow* w = ImGui::FindWindowByName(siblingWindow)) {
        dock = w->DockId;
    }
    if (dock == 0) {
        // The sibling has not been drawn yet this session (the app started in the Earth
        // view): its dock node is still in the settings loaded from imgui.ini.
        if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(siblingWindow))) {
            dock = settings->DockId;
        }
    }
    if (dock != 0) {
        ImGui::SetNextWindowDockID(dock, ImGuiCond_FirstUseEver);
    }
}

// A micro label and a value on one row.
void dataRow(const char* label, const char* value, ImU32 valueColour) {
    const float s = dpi();
    TinyText(label);
    ImGui::SameLine(112.0f * s);
    TinyText(value, valueColour);
}

// Distance in lunar distances and km, from an AU value.
std::string distanceText(double au) {
    char buf[96];
    const double km = au * sim::kAU_km;
    std::snprintf(buf, sizeof buf, "%.3f LD \xC2\xB7 %s KM", au / neo::kLunarDistanceAU,
                  thousands(static_cast<std::size_t>(std::llround(km))).c_str());
    return buf;
}

const neo::SortField kSortFields[] = {neo::SortField::Distance, neo::SortField::Date, neo::SortField::Velocity,
                                      neo::SortField::Diameter, neo::SortField::AbsoluteMagnitude,
                                      neo::SortField::Moid};
const char* const kSortLabels[] = {"DISTANCE", "DATE", "V_REL", "DIAMETER", "H", "MOID"};
const char* const kModeLabels[] = {"MEASURED + EST.", "MEASURED ONLY", "INCL. UNKNOWN"};

int sortIndexOf(neo::SortField f) {
    for (int i = 0; i < 6; ++i) {
        if (kSortFields[i] == f) {
            return i;
        }
    }
    return 0;
}

int modeIndexOf(neo::DiameterMode m) {
    return m == neo::DiameterMode::MeasuredOrEstimated ? 0 : (m == neo::DiameterMode::MeasuredOnly ? 1 : 2);
}
neo::DiameterMode modeFromIndex(int i) {
    return i == 0 ? neo::DiameterMode::MeasuredOrEstimated
                  : (i == 1 ? neo::DiameterMode::MeasuredOnly : neo::DiameterMode::IncludeUnknown);
}

// Two number inputs on one row, "0 = no limit".
bool rangeRow(const char* id, float* lo, float* hi, float width) {
    bool changed = false;
    const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    char label[32];
    ImGui::SetNextItemWidth(half);
    std::snprintf(label, sizeof label, "##%s_lo", id);
    changed |= ImGui::InputFloat(label, lo, 0.0f, 0.0f, "%g");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    std::snprintf(label, sizeof label, "##%s_hi", id);
    changed |= ImGui::InputFloat(label, hi, 0.0f, 0.0f, "%g");
    return changed;
}

} // namespace

// ---------------------------------------------------------------------------
// NEO FILTER
// ---------------------------------------------------------------------------

void drawNeoFilterPanel(EarthUiState& ui, const NeoPanelView& view, HudEvents& ev) {
    dockBeside(kWinLog);
    const HudTheme& t = theme();
    const char* tag = view.db == NeoPanelView::Db::Ready ? (view.busy ? "RUNNING" : "READY")
                                                          : (view.db == NeoPanelView::Db::Failed ? "NO DB" : "LOADING");
    if (BeginPanel(kWinFilter, "NEO FILTER", tag)) {
        neo::NeoFilterState& f = ui.filter;
        const float s = dpi();
        const float width = ImGui::GetContentRegionAvail().x;

        // RUN and its result come first: the form below can be long, RUN must never scroll away.
        pushFontSize(t.sizeSmall);
        const bool ready = view.db == NeoPanelView::Db::Ready;
        ImGui::BeginDisabled(!ready);
        if (Button(view.busy ? "RUNNING..." : "RUN", 0.0f, true)) {
            ev.runQuery = true;
        }
        ImGui::EndDisabled();
        popFont();
        ImGui::SameLine();

        // Database status, then the last result and its EXPLAIN.
        ImGui::BeginGroup();
        if (view.db == NeoPanelView::Db::Ready) {
            char line[160];
            std::snprintf(line, sizeof line, "%s OBJECTS \xC2\xB7 %s APPROACHES MATCHED \xC2\xB7 %s SHOWN",
                          thousands(view.matchedObjects).c_str(), thousands(view.matchedApproaches).c_str(),
                          thousands(view.scene ? view.scene->flybys.size() : 0).c_str());
            TinyText(view.scene ? line : "NO RESULT YET \xC2\xB7 PRESS RUN", t.textValue);
        } else {
            TinyText(view.dbMessage, view.db == NeoPanelView::Db::Failed ? t.alarm : t.textLabel);
        }
        ImGui::EndGroup();

        if (view.summary && view.summary[0] != '\0') {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textMicro));
            pushFontSize(t.sizeMicro);
            ImGui::TextUnformatted(view.summary);
            popFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        if (view.scene && view.scene->truncated() > 0) {
            char note[96];
            std::snprintf(note, sizeof note, "%s APPROACHES NOT DRAWN (1,000 FLYBY CAP)",
                          thousands(view.scene->truncated()).c_str());
            TinyText(note, t.alarm);
        }
        if (view.errors) {
            for (const std::string& e : *view.errors) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.alarm));
                pushFontSize(t.sizeLabel);
                ImGui::TextUnformatted(e.c_str());
                popFont();
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
        pushFontSize(t.sizeLabel);

        TinyText("DATE WINDOW  (YYYY-MM-DD, EMPTY = OPEN)");
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(half);
        ImGui::InputText("##from", f.dateFrom, sizeof f.dateFrom);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half);
        ImGui::InputText("##to", f.dateTo, sizeof f.dateTo);

        ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
        TinyText("MAX DISTANCE  (0 = NO LIMIT)");
        ImGui::SetNextItemWidth(half);
        ImGui::InputFloat("##maxd", &f.maxDistance, 0.0f, 0.0f, "%g");
        ImGui::SameLine();
        const bool ld = f.distanceUnit == neo::DistanceUnit::LunarDistance;
        const float unitW = (half - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (Button("LD", unitW, ld)) {
            neo::changeDistanceUnit(f, neo::DistanceUnit::LunarDistance); // the same distance, in the new unit
        }
        ImGui::SameLine();
        if (Button("AU", unitW, !ld)) {
            neo::changeDistanceUnit(f, neo::DistanceUnit::Au);
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
        TinyText("DIAMETER  MIN / MAX  (METRES, 0 = NO LIMIT)");
        rangeRow("diam", &f.minDiameterM, &f.maxDiameterM, width);
        int mode = modeIndexOf(f.diameterMode);
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo("##diammode", &mode, kModeLabels, 3)) {
            f.diameterMode = modeFromIndex(mode);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Estimated diameters come from the absolute magnitude H (albedo 0.14) and can be\n"
                              "off by a factor of about two. Estimated markers are drawn hollow.");
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
        TinyText("V_REL  MIN / MAX  (KM/S, 0 = NO LIMIT)");
        rangeRow("vel", &f.minVelocity, &f.maxVelocity, width);

        ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
        Checkbox("PHA ONLY", &f.phaOnly);
        ImGui::SameLine();
        Checkbox("GRAZING ONLY", &f.grazingOnly);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Nominal distance below one Earth radius (a graze or an impact).\n"
                              "The dataset currently has none: the closest pass is 1.03 radii.");
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
        TinyText("SORT BY  \xC2\xB7  TOP-K");
        int sort = sortIndexOf(f.sortBy);
        ImGui::SetNextItemWidth(half * 0.9f);
        if (ImGui::Combo("##sort", &sort, kSortLabels, 6)) {
            f.sortBy = kSortFields[sort];
        }
        ImGui::SameLine();
        const bool asc = f.direction == neo::SortDirection::Ascending;
        if (Button(asc ? "ASC" : "DESC", 0.0f, false)) {
            f.direction = asc ? neo::SortDirection::Descending : neo::SortDirection::Ascending;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("##topk", &f.topK, 0, 0);
        f.topK = std::clamp(f.topK, 1, neo::kMaxTopK);
        popFont();

    }
    EndPanel();
}

// ---------------------------------------------------------------------------
// EARTH VIEW controls and legend
// ---------------------------------------------------------------------------

void drawEarthViewPanel(EarthUiState& ui, const NeoPanelView& view, HudEvents& ev) {
    dockBeside(kWinTelemetry);
    const HudTheme& t = theme();
    if (BeginPanel(kWinEarth, "EARTH VIEW", view.textureLoaded ? "BLUE MARBLE" : "GRID")) {
        const float s = dpi();
        const float width = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        pushFontSize(t.sizeLabel);

        TinyText("EARTH ROTATION  \xC2\xB7  23.44\xC2\xB0 TILT  \xC2\xB7  INDEPENDENT OF THE SIM CLOCK");
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.2fx  \xC2\xB7  1 TURN / %.0f S", static_cast<double>(ui.spinSpeed),
                      static_cast<double>(kSpinSecondsPerTurn / std::max(ui.spinSpeed, 0.01f)));
        ImGui::SetNextItemWidth(width - 80.0f * s - spacing);
        ImGui::SliderFloat("##spin", &ui.spinSpeed, 0.0f, 4.0f, buf, ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (Button(ui.spinPaused ? "SPIN" : "PAUSE", 0.0f, ui.spinPaused)) {
            ui.spinPaused = !ui.spinPaused;
        }

        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
        TinyText("FLYBYS  (THE SIM CLOCK MOVES THEM)");
        const float bw = (width - 2.0f * spacing) / 3.0f;
        const bool haveScene = view.scene != nullptr && !view.scene->flybys.empty();
        ImGui::BeginDisabled(!haveScene);
        if (Button("PREV RESULT", bw)) {
            ev.resultPrev = true;
        }
        ImGui::SameLine();
        if (Button("NEXT RESULT", bw)) {
            ev.resultNext = true;
        }
        ImGui::SameLine();
        if (Button("NEXT APPROACH", bw, false)) {
            ev.jumpNextApproach = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Jump the simulation clock to the next flyby's closest approach");
        }

        if (haveScene) {
            const int next = neo::nextFlyby(*view.scene, view.jdNow);
            if (next >= 0 && view.dataset) {
                const neo::Flyby& f = view.scene->flybys[static_cast<std::size_t>(next)];
                std::snprintf(buf, sizeof buf, "NEXT  %s  \xC2\xB7  %s  \xC2\xB7  IN %.1f D",
                              view.dataset->records()[f.object].object.pdes.c_str(), neo::formatJulianDay(f.tcaJd).c_str(),
                              f.tcaJd - view.jdNow);
                TinyText(buf, t.textValue);
            } else {
                TinyText("NO LATER APPROACH IN THIS RESULT", t.textLabel);
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
        Checkbox("REFERENCE RINGS", &ui.showRings);

        // The scale, stated: a log radial scale is only honest if it is declared. (It is also in
        // the viewport's top-right corner, so it is on screen whichever panel is open.)
        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
        const neo::EarthViewScale scale = view.scene ? view.scene->scale : neo::EarthViewScale();
        std::snprintf(buf, sizeof buf, "SCALE LOG10:  r = 1 + %.1f\xC2\xB7log10(d / R)   R = EARTH RADIUS = 1 U",
                      scale.logSlope);
        TinyText(buf, t.accent);
        std::snprintf(buf, sizeof buf, "GEO %.1f \xC2\xB7 1 LD %.1f \xC2\xB7 5 LD %.1f \xC2\xB7 0.05 AU %.1f   (U)",
                      neo::radialFromKm(42164.0, scale), neo::radialFromKm(384400.0, scale),
                      neo::radialFromKm(5.0 * 384400.0, scale), neo::radialFromKm(0.05 * sim::kAU_km, scale));
        TinyText(buf, t.textValue);
        std::snprintf(buf, sizeof buf, "REAL: DATE, DISTANCE, V_REL, SIZE   [%.2f U/DAY PER KM/S]",
                      scale.unitsPerDayPerKms);
        TinyText(buf, t.textLabel);
        TinyText("SCHEMATIC: DIRECTION (DESIGNATION HASH), STRAIGHT PATH", t.alarm);
        TinyText("HOLLOW = D ESTIMATED FROM H  \xC2\xB7  ACCENT = PHA", t.textLabel);
        popFont();
    }
    EndPanel();
}

// ---------------------------------------------------------------------------
// NEO RESULTS
// ---------------------------------------------------------------------------

void drawNeoResultsPanel(EarthUiState& ui, const NeoPanelView& view, HudEvents& ev) {
    dockBeside(kWinHistory);
    const HudTheme& t = theme();
    char tag[48] = "NO RESULT";
    if (view.scene) {
        std::snprintf(tag, sizeof tag, "%s FLYBYS", thousands(view.scene->flybys.size()).c_str());
    }
    if (BeginPanel(kWinResults, "NEO RESULTS", tag)) {
        const float s = dpi();
        if (!view.scene || !view.dataset) {
            TinyText(view.db == NeoPanelView::Db::Ready ? "PRESS RUN IN NEO FILTER TO SEE APPROACHES"
                                                          : "THE NEO DATABASE IS NOT LOADED",
                     t.textLabel);
            EndPanel();
            return;
        }
        if (view.scene->flybys.empty()) {
            TinyText("NO APPROACH MATCHES THESE FILTERS", t.textLabel);
            EndPanel();
            return;
        }
        pushFontSize(t.sizeLabel);
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f * s, 1.5f * s));
        const bool tableOpen = ImGui::BeginTable("##neo_results", 6, flags, ImVec2(0.0f, 0.0f));
        if (tableOpen) {
            ImGui::TableSetupScrollFreeze(0, 1);
            // Six narrow columns: the rest of an asteroid (H, orbit class, MOID, min/max distance)
            // is in TARGET / OBSERVATION. A PHA row is drawn in the accent colour.
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22.0f * s);
            ImGui::TableSetupColumn("DESIGNATION", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("D KM", ImGuiTableColumnFlags_WidthFixed, 46.0f * s);
            ImGui::TableSetupColumn("DATE", ImGuiTableColumnFlags_WidthFixed, 68.0f * s);
            ImGui::TableSetupColumn("LD", ImGuiTableColumnFlags_WidthFixed, 42.0f * s);
            ImGui::TableSetupColumn("KM/S", ImGuiTableColumnFlags_WidthFixed, 38.0f * s);
            ImGui::TableHeadersRow();

            const std::vector<neo::Flyby>& flybys = view.scene->flybys;
            const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;

            // Keep the selected row in view when the selection changes elsewhere
            // (a click on a marker), but never fight the user's own scrolling.
            static int lastSelected = -2;
            if (ui.selected != lastSelected) {
                lastSelected = ui.selected;
                if (ui.selected >= 0) {
                    const float top = static_cast<float>(ui.selected) * rowH;
                    const float viewH = ImGui::GetWindowHeight() - rowH * 2.0f;
                    if (top < ImGui::GetScrollY() || top > ImGui::GetScrollY() + viewH - rowH) {
                        ImGui::SetScrollY(std::max(0.0f, top - viewH * 0.4f));
                    }
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(flybys.size()), rowH);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const neo::Flyby& f = flybys[static_cast<std::size_t>(i)];
                    const neo::Asteroid& a = view.dataset->records()[f.object].object;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    char cell[64];
                    std::snprintf(cell, sizeof cell, "%d", i + 1);
                    const bool selected = i == ui.selected;
                    // A selectable spanning the whole row makes it clickable.
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(f.pha ? t.accent : t.textValue));
                    char id[24];
                    std::snprintf(id, sizeof id, "%s##row%d", cell, i);
                    if (ImGui::Selectable(id, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        ev.rowClicked = i;
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ui.hovered = i;
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(a.label().c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (a.physical.diameterKm) {
                        std::snprintf(cell, sizeof cell, "%.3f", *a.physical.diameterKm);
                    } else if (const std::optional<double> est = a.physical.estimatedDiameterKm()) {
                        std::snprintf(cell, sizeof cell, "~%.3f", *est); // '~' = estimated from H
                    } else {
                        std::snprintf(cell, sizeof cell, "?");
                    }
                    ImGui::TextUnformatted(cell);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(neo::formatJulianDay(f.tcaJd).c_str());
                    ImGui::TableSetColumnIndex(4);
                    std::snprintf(cell, sizeof cell, "%.3f", f.distanceAU / neo::kLunarDistanceAU);
                    ImGui::TextUnformatted(cell);
                    ImGui::TableSetColumnIndex(5);
                    std::snprintf(cell, sizeof cell, "%.2f", f.vRelKms);
                    ImGui::TextUnformatted(cell);
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        popFont();
    }
    EndPanel();
}

// ---------------------------------------------------------------------------
// TARGET · OBSERVATION for a flyby
// ---------------------------------------------------------------------------

void drawAsteroidTarget(const EarthUiState& ui, const NeoPanelView& view) {
    const bool has = view.scene && view.dataset && ui.selected >= 0 &&
                     static_cast<std::size_t>(ui.selected) < view.scene->flybys.size();
    const neo::Flyby* f = has ? &view.scene->flybys[static_cast<std::size_t>(ui.selected)] : nullptr;
    const neo::AsteroidRecord* record = has ? &view.dataset->records()[f->object] : nullptr;
    char hex[16] = "";
    if (has) {
        std::snprintf(hex, sizeof hex, "0x%04X", hexId(record->object.pdes.c_str()));
    }
    if (BeginPanel(kWinTarget, "TARGET \xC2\xB7 OBSERVATION", has ? "NEO FLYBY" : "EARTH VIEW")) {
        if (!has) {
            TinyText("CLICK A MARKER (OR A RESULTS ROW) TO SEE THE ASTEROID");
            EndPanel();
            return;
        }
        const HudTheme& t = theme();
        const float s = dpi();
        const neo::Asteroid& a = record->object;
        const neo::CloseApproach& ap = view.dataset->approaches()[f->approach];

        // Name row.
        pushFontSize(t.sizeValue);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textHero));
        ImGui::TextUnformatted(a.label().c_str());
        ImGui::PopStyleColor();
        popFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * s);
        if (a.classification.isPHA && *a.classification.isPHA) {
            Chip("PHA", ChipTone::Alarm);
            ImGui::SameLine();
        } else if (a.classification.isNEO && *a.classification.isNEO) {
            Chip("NEO", ChipTone::Accent);
            ImGui::SameLine();
        }
        Chip(hex, ChipTone::Micro);
        if (!a.fullName.empty()) {
            std::string full = a.fullName;
            full.erase(0, full.find_first_not_of(' '));
            TinyText(full.c_str());
        }

        // Highlighted metric: the nominal distance in lunar distances.
        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
        HeroNumber(f->distanceAU / neo::kLunarDistanceAU, 2, 2, "LD \xC2\xB7 NOMINAL CLOSEST APPROACH");
        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));

        char buf[128];
        dataRow("DATE (TDB)", neo::formatJulianDate(f->tcaJd).c_str(), t.textValue);
        dataRow("NOMINAL", distanceText(ap.distanceAU).c_str(), t.textValue);
        dataRow("MIN (3\xCF\x83)", distanceText(ap.distanceMinAU).c_str(), t.textValue);
        dataRow("MAX (3\xCF\x83)", distanceText(ap.distanceMaxAU).c_str(), t.textValue);
        if (ap.distRangeDerived) {
            TinyText("  MIN/MAX WERE ABSENT: THE NOMINAL DISTANCE STANDS IN", t.textMicro);
        }
        std::snprintf(buf, sizeof buf, "%.2f KM/S", ap.relVelocityKms);
        dataRow("V_REL", buf, t.textValue);
        if (ap.vInfinityKms) {
            std::snprintf(buf, sizeof buf, "%.2f KM/S", *ap.vInfinityKms);
            dataRow("V_INF", buf, t.textValue);
        } else {
            dataRow("V_INF", "NOT PROVIDED", t.textLabel);
        }

        if (a.physical.diameterKm) {
            std::snprintf(buf, sizeof buf, "%.3f KM \xC2\xB7 MEASURED", *a.physical.diameterKm);
            dataRow("DIAMETER", buf, t.textValue);
        } else if (const std::optional<double> est = a.physical.estimatedDiameterKm()) {
            std::snprintf(buf, sizeof buf, "~%.3f KM \xC2\xB7 ESTIMATED FROM H", *est);
            dataRow("DIAMETER", buf, t.accent);
        } else {
            dataRow("DIAMETER", "UNKNOWN", t.textLabel);
        }
        if (a.physical.absoluteMagnitudeH) {
            std::snprintf(buf, sizeof buf, "%.2f", *a.physical.absoluteMagnitudeH);
            dataRow("H", buf, t.textValue);
        } else {
            dataRow("H", "UNKNOWN", t.textLabel);
        }
        dataRow("PHA", !a.classification.isPHA ? "UNKNOWN" : (*a.classification.isPHA ? "YES" : "NO"), t.textValue);
        if (!a.classification.orbitClass.empty()) {
            dataRow("ORBIT CLASS", a.classification.orbitClass.c_str(), t.textValue);
        }
        std::snprintf(buf, sizeof buf, "%u OF %u ON RECORD", f->ordinal + 1, record->approachCount);
        dataRow("APPROACH", buf, t.textValue);
        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
        TinyText("PATH DIRECTION IS SCHEMATIC (DESIGNATION HASH)", t.textMicro);
    }
    EndPanel();
}

} // namespace hud
