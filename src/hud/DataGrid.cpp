#include "hud/Panels.h"
#include "hud/Theme.h"
#include "sim/Constants.h"
#include "sim/SolarSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hud {

namespace {

// Largest eccentricity among the planets (Mercury): full scale of the ECC gauge.
constexpr double kMaxPlanetEcc = 0.2056;
// Within this fraction of the q..Q range from either end, the body is "at" perihelion/aphelion.
constexpr double kExtremeBand = 0.03;

struct Cell {
    const char* label;
    double v;
    const char* fmt;
    const char* unit;
};

enum class Extreme { None, Peri, Apo };

// Gauge values shared by both layouts.
struct Gauges {
    float orbitFrac, eccFrac, rangeFrac;
    char orbit[16], ecc[16], range[16];
};

Gauges gaugesFor(const sim::Body& b) {
    const sim::OrbitState& o = b.orbit;
    const sim::OrbitalElements& el = b.data().elements;
    Gauges g{};
    const double m = o.M < 0.0 ? o.M + sim::kTwoPi : o.M;
    g.orbitFrac = static_cast<float>(m / sim::kTwoPi);
    g.eccFrac = static_cast<float>(el.e / kMaxPlanetEcc);
    const double q = el.perihelion_AU();
    const double Q = el.aphelion_AU();
    g.rangeFrac = Q > q ? static_cast<float>((o.r_AU - q) / (Q - q)) : 0.0f;
    std::snprintf(g.orbit, sizeof g.orbit, "%.0f%%", 100.0 * static_cast<double>(g.orbitFrac));
    std::snprintf(g.ecc, sizeof g.ecc, "%.3f", el.e);
    std::snprintf(g.range, sizeof g.range, "%.0f%%", 100.0 * static_cast<double>(std::clamp(g.rangeFrac, 0.0f, 1.0f)));
    return g;
}

// --- OBSERVATORY: 2x4 blocks + ring gauges ----------------------------------------------

// One block of the 2x4 grid: data marker + tiny label, value, unit and hex tag.
void dataBlock(const char* label, const char* value, const char* unit, unsigned hex, float width) {
    const HudTheme& t = theme();
    const float s = dpi();
    ImFont* f = regularFont();
    const float lab = t.sizeLabel * s;
    const float val = 13.5f * s;
    const float pad = 5.0f * s;
    const float h = pad + lab + 3.0f * s + val + 3.0f * s + lab + pad;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + width, p.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, t.block);
    dl->AddRect(p, q, t.line, 0.0f, 0, 1.0f);

    float y = p.y + pad;
    const float x0 = p.x + pad;
    dl->AddRectFilled(ImVec2(x0, y + 0.5f * lab - 2.0f * s), ImVec2(x0 + 4.0f * s, y + 0.5f * lab + 2.0f * s), t.data);
    dl->AddText(f, lab, ImVec2(x0 + 8.0f * s, y), t.textLabel, label);
    y += lab + 3.0f * s;
    dl->AddText(f, val, ImVec2(x0, y), t.textValue, value);
    y += val + 3.0f * s;
    char tag[32];
    std::snprintf(tag, sizeof tag, "%s  0x%04X", unit, hex);
    dl->AddText(f, lab, ImVec2(x0, y), t.textMicro, tag);
    ImGui::Dummy(ImVec2(width, h));
}

void blocksAndRings(const sim::Body& b, const Cell (&cells)[8]) {
    const HudTheme& t = theme();
    const float s = dpi();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float cw = (ImGui::GetContentRegionAvail().x - 3.0f * spacing) / 4.0f;
    for (int i = 0; i < 8; ++i) {
        if (i % 4 != 0) {
            ImGui::SameLine();
        }
        char value[32];
        std::snprintf(value, sizeof value, cells[i].fmt, cells[i].v);
        dataBlock(cells[i].label, value, cells[i].unit, hexId(cells[i].v), cw);
    }

    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
    const Gauges g = gaugesFor(b);
    const float diameter = 54.0f;
    const float cell = ImGui::GetContentRegionAvail().x / 3.0f;
    const float startX = ImGui::GetCursorPosX();
    const char* labels[3] = {"ORBIT", "ECC / MAX", "PERI > APO"};
    const float fracs[3] = {g.orbitFrac, g.eccFrac, g.rangeFrac};
    const char* centres[3] = {g.orbit, g.ecc, g.range};
    const ImU32 cols[3] = {t.accent, t.data, t.accent};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        // Centre each gauge in its third of the row.
        const float gaugeW = std::max(diameter * s, ImGui::CalcTextSize(labels[i]).x);
        ImGui::SetCursorPosX(startX + cell * static_cast<float>(i) + 0.5f * (cell - gaugeW));
        RingGauge(labels[i], fracs[i], centres[i], diameter, cols[i]);
    }
}

// --- REACTOR: dense table + tall bar gauges -----------------------------------------------

void rightAligned(const char* text, ImU32 c) {
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetColumnWidth() - w));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(c));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void tableAndBars(const sim::Body& b, const Cell (&cells)[8]) {
    const HudTheme& t = theme();
    const float s = dpi();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float tableW = std::floor(avail * 0.74f);

    // Where the body sits between perihelion and aphelion drives the status cells.
    const sim::OrbitalElements& el = b.data().elements;
    const double q = el.perihelion_AU();
    const double Q = el.aphelion_AU();
    const double rangePos = Q > q ? (b.orbit.r_AU - q) / (Q - q) : 0.5;
    const Extreme ext = rangePos < kExtremeBand ? Extreme::Peri
                      : rangePos > 1.0 - kExtremeBand ? Extreme::Apo
                                                      : Extreme::None;

    ImGui::BeginGroup();
    pushFontSize(t.sizeLabel);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuterH |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("##statevec", 5, flags, ImVec2(tableW, 0.0f))) {
        ImGui::TableSetupColumn("PARAM", ImGuiTableColumnFlags_WidthFixed, 58.0f * s);
        ImGui::TableSetupColumn("VALUE", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("UNIT", ImGuiTableColumnFlags_WidthFixed, 26.0f * s);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 26.0f * s);
        ImGui::TableSetupColumn("STAT", ImGuiTableColumnFlags_WidthFixed, 30.0f * s);

        // Header row in micro text.
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        const char* heads[5] = {"PARAM", "VALUE", "UNIT", "ID", "STAT"};
        pushFontSize(t.sizeMicro);
        for (int c = 0; c < 5; ++c) {
            ImGui::TableSetColumnIndex(c);
            if (c == 1) {
                rightAligned(heads[c], t.textMicro);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textMicro));
                ImGui::TextUnformatted(heads[c]);
                ImGui::PopStyleColor();
            }
        }
        popFont();

        for (int i = 0; i < 8; ++i) {
            const Cell& cell = cells[i];
            char value[32];
            std::snprintf(value, sizeof value, cell.fmt, cell.v);
            char hex[12];
            std::snprintf(hex, sizeof hex, "%04X", hexId(cell.v));
            const bool anomalyRow = i >= 6; // MEAN / TRUE anomaly rows carry the PERI/APO status

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textLabel));
            ImGui::TextUnformatted(cell.label);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            rightAligned(value, anomalyRow && ext != Extreme::None ? t.textHero : t.textValue);

            ImGui::TableSetColumnIndex(2);
            pushFontSize(t.sizeMicro);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textMicro));
            ImGui::TextUnformatted(cell.unit);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(hex);
            ImGui::PopStyleColor();
            popFont();

            ImGui::TableSetColumnIndex(4);
            if (anomalyRow && ext != Extreme::None) {
                Chip(ext == Extreme::Peri ? "PERI" : "APO", ChipTone::Alarm, true);
            } else {
                pushFontSize(t.sizeMicro);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textMicro));
                ImGui::TextUnformatted(anomalyRow ? "NOM" : "--");
                ImGui::PopStyleColor();
                popFont();
            }
        }
        ImGui::EndTable();
    }
    popFont();
    ImGui::EndGroup();
    const float tableH = ImGui::GetItemRectSize().y;

    // Three tall bar gauges beside the table, as high as the table.
    ImGui::SameLine(0.0f, 2.0f * spacing);
    const Gauges g = gaugesFor(b);
    const float gw = (ImGui::GetContentRegionAvail().x - 2.0f * spacing) / 3.0f;
    BarGauge("ORB", g.orbit, g.orbitFrac, gw, tableH);
    ImGui::SameLine();
    BarGauge("ECC", g.ecc, g.eccFrac, gw, tableH);
    ImGui::SameLine();
    BarGauge("R\xC2\xB7Q", g.range, g.rangeFrac, gw, tableH, ext != Extreme::None);
}

} // namespace

void drawDataGrid(const sim::SolarSystem& system, const HudState& state) {
    const HudTheme& t = theme();
    if (BeginPanel(kWinTelemetry, "STATE VECTOR", "ECL \xC2\xB7 J2000")) {
        const bool has = state.selected >= 0 && state.selected < system.bodyCount();
        if (!has || system.body(state.selected).parentIndex < 0) {
            TinyText(has ? "SUN: HELIOCENTRIC ORIGIN, STATE VECTOR IS ZERO" : "NO TARGET");
            EndPanel();
            return;
        }
        const sim::Body& b = system.body(state.selected);
        const sim::OrbitState& o = b.orbit;
        const Cell cells[8] = {
            {"POS X", b.helioPos_AU.x, "%+.5f", "AU"},
            {"POS Y", b.helioPos_AU.y, "%+.5f", "AU"},
            {"POS Z", b.helioPos_AU.z, "%+.5f", "AU"},
            {"VEL X", b.helioVel_kms.x, "%+.4f", "KM/S"},
            {"VEL Y", b.helioVel_kms.y, "%+.4f", "KM/S"},
            {"VEL Z", b.helioVel_kms.z, "%+.4f", "KM/S"},
            {"MEAN ANOM", o.M * sim::kRadToDeg, "%+.3f", "DEG"},
            {"TRUE ANOM", o.nu * sim::kRadToDeg, "%+.3f", "DEG"},
        };
        if (t.denseTable) {
            tableAndBars(b, cells);
        } else {
            // Original 2x4 order: positions + mean anomaly, then velocities + true anomaly.
            const Cell grid[8] = {cells[0], cells[1], cells[2], cells[6], cells[3], cells[4], cells[5], cells[7]};
            blocksAndRings(b, grid);
        }
    }
    EndPanel();
}

} // namespace hud
