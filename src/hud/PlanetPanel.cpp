#include "hud/Panels.h"
#include "hud/Theme.h"
#include "sim/BodyTable.h"
#include "sim/Constants.h"
#include "sim/KeplerSolver.h"
#include "sim/SolarSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace hud {

namespace {

// Range of each readout across the eight planets (from the body table), so a
// bar shows where the selected body sits among its peers.
struct Range {
    double lo = 1e300;
    double hi = -1e300;
    void add(double v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
};

struct PlanetRanges {
    Range mass, radius, distance, speed, period, ecc, incl;
};

const PlanetRanges& planetRanges() {
    static const PlanetRanges ranges = [] {
        PlanetRanges r;
        for (int i = 0; i < sim::bodyTableSize(); ++i) {
            const sim::BodyData& d = sim::bodyData(i);
            if (d.kind != sim::BodyKind::Planet) {
                continue;
            }
            const sim::OrbitalElements& el = d.elements;
            r.mass.add(d.mass_kg);
            r.radius.add(d.radius_km);
            r.distance.add(el.perihelion_AU());
            r.distance.add(el.aphelion_AU());
            r.speed.add(sim::visVivaSpeedKms(el.perihelion_AU(), el.a_AU, sim::kMuSun_m3s2));
            r.speed.add(sim::visVivaSpeedKms(el.aphelion_AU(), el.a_AU, sim::kMuSun_m3s2));
            r.period.add(sim::orbitalPeriodDays(el.a_AU, sim::kMuSun_m3s2));
            r.ecc.add(el.e);
            r.incl.add(std::fabs(el.i_deg));
        }
        return r;
    }();
    return ranges;
}

float linFrac(double v, const Range& r) { return static_cast<float>((v - r.lo) / (r.hi - r.lo)); }

// Quantities spanning orders of magnitude (mass, radius, distance, period) use a log bar.
float logFrac(double v, const Range& r) {
    return static_cast<float>((std::log10(v) - std::log10(r.lo)) / (std::log10(r.hi) - std::log10(r.lo)));
}

bool outside(double v, const Range& r) { return v < r.lo * 0.999 || v > r.hi * 1.001; }

// Compact engineering formatting for min/max captions: 3.3E23, 1.9E27, 2440, 0.31 ...
void compact(char* buf, std::size_t n, double v) {
    if (v != 0.0 && (std::fabs(v) >= 1e5 || std::fabs(v) < 1e-2)) {
        std::snprintf(buf, n, "%.1E", v);
    } else if (std::fabs(v) >= 100.0) {
        std::snprintf(buf, n, "%.0f", v);
    } else {
        std::snprintf(buf, n, "%.2f", v);
    }
}

struct Readout {
    const char* label;
    char value[48];
    char lo[24];
    char hi[24];
    float frac;
    bool out;
};

Readout make(const char* label, double v, const Range& r, bool logScale, const char* valueFmt) {
    Readout o{};
    o.label = label;
    std::snprintf(o.value, sizeof o.value, valueFmt, v);
    compact(o.lo, sizeof o.lo, r.lo);
    compact(o.hi, sizeof o.hi, r.hi);
    o.frac = logScale ? logFrac(v, r) : linFrac(v, r);
    o.out = outside(v, r);
    return o;
}

void readoutGrid(const Readout* items, int count) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float colW = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
    for (int i = 0; i < count; ++i) {
        if (i % 2 == 1) {
            ImGui::SameLine();
        }
        const Readout& r = items[i];
        const bool lastAlone = i == count - 1 && i % 2 == 0;
        ReadoutBar(r.label, r.value, r.frac, r.lo, r.hi, lastAlone ? 0.0f : colW, r.out);
    }
}

// Right of the hero number: a short list in the style of an observation record.
// `compact` lays the four lines out as a 2x2 block (used inside the callout).
void orbitList(const sim::Body& b, double elapsedDays, bool compact) {
    const sim::OrbitalElements& el = b.data().elements;
    const double period = sim::kTwoPi / b.meanMotion_radPerDay;
    // Compact lines drop the unit (it moves into the title) so two columns fit.
    const char* au = compact ? "" : " AU";
    char lines[4][40];
    if (compact) {
        std::snprintf(lines[0], sizeof lines[0], "\xC2\xB7 a %.4f%s", el.a_AU, au);
        std::snprintf(lines[1], sizeof lines[1], "\xC2\xB7 q %.4f%s", el.perihelion_AU(), au);
        std::snprintf(lines[2], sizeof lines[2], "\xC2\xB7 Q %.4f%s", el.aphelion_AU(), au);
        std::snprintf(lines[3], sizeof lines[3], "\xC2\xB7 N %.3f", elapsedDays / period);
    } else {
        std::snprintf(lines[0], sizeof lines[0], "\xC2\xB7 a  %8.4f AU", el.a_AU);
        std::snprintf(lines[1], sizeof lines[1], "\xC2\xB7 q  %8.4f AU", el.perihelion_AU());
        std::snprintf(lines[2], sizeof lines[2], "\xC2\xB7 Q  %8.4f AU", el.aphelion_AU());
        std::snprintf(lines[3], sizeof lines[3], "\xC2\xB7 N  %8.3f REV", elapsedDays / period);
    }
    ImGui::BeginGroup();
    TinyText(compact ? "ORBIT RECORD \xC2\xB7 AU" : "ORBIT RECORD", theme().accent);
    if (compact) {
        pushFontSize(theme().sizeLabel);
        float colW = 0.0f;
        for (const char* line : lines) {
            colW = std::max(colW, ImGui::CalcTextSize(line).x);
        }
        popFont();
        colW += 12.0f * dpi();
        for (int i = 0; i < 4; ++i) {
            if (i % 2 == 1) {
                ImGui::SameLine(colW); // inside a group, the offset is relative to the group start
            }
            TinyText(lines[i], theme().textValue);
        }
    } else {
        for (const char* line : lines) {
            TinyText(line, theme().textValue);
        }
    }
    ImGui::EndGroup();
}

} // namespace

void drawPlanetPanel(const sim::SolarSystem& system, const HudState& state, double elapsedDays) {
    const bool has = state.selected >= 0 && state.selected < system.bodyCount();
    const char* tag = has ? system.body(state.selected).data().idTag : "NO TARGET";
    if (BeginPanel(kWinTarget, "TARGET \xC2\xB7 OBSERVATION", tag)) {
        if (!has) {
            TinyText("CLICK A BODY IN THE VIEW TO SELECT IT");
            EndPanel();
            return;
        }
        const HudTheme& t = theme();
        const float s = dpi();
        const sim::Body& b = system.body(state.selected);
        const sim::BodyData& d = b.data();
        const PlanetRanges& R = planetRanges();

        // Name row: name, class chip, hex ID chip.
        pushFontSize(t.sizeValue);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textHero));
        ImGui::TextUnformatted(d.name);
        ImGui::PopStyleColor();
        popFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * s);
        Chip(d.kind == sim::BodyKind::Star ? "STAR" : "PLANET", ChipTone::Accent);
        ImGui::SameLine();
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%04X", hexId(d.name));
        Chip(hex, ChipTone::Micro);
        if (state.following) {
            ImGui::SameLine();
            Chip("TRACKING", ChipTone::Accent, true);
        }

        if (b.parentIndex < 0) {
            // The Sun: no orbit. Show its size against the planets (out of range = amber).
            HeroNumber(d.radius_km / 1000.0, 1, 0, "10^3 KM \xC2\xB7 MEAN RADIUS");
            TinyText("HELIOCENTRIC ORIGIN \xC2\xB7 NO ORBIT");
            const Readout items[] = {
                make("MASS \xC2\xB7 KG", d.mass_kg, R.mass, true, "%.3E"),
                make("RADIUS \xC2\xB7 KM", d.radius_km, R.radius, true, "%.0f"),
            };
            readoutGrid(items, 2);
        } else {
            const double speed = sim::length(b.orbit.vel_kms);
            const double period = sim::kTwoPi / b.meanMotion_radPerDay;

            // Highlighted metric: hero distance + orbit record, inside a dashed
            // callout box in themes that use one.
            const float calloutRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - 1.0f * s;
            if (t.dashedCallout) {
                ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
                ImGui::Indent(7.0f * s);
            }
            ImGui::BeginGroup();
            HeroNumber(b.orbit.r_AU, 2, 2, "AU \xC2\xB7 FROM SUN");
            ImGui::SameLine(0.0f, 18.0f * s);
            orbitList(b, elapsedDays, t.dashedCallout);
            ImGui::EndGroup();
            if (t.dashedCallout) {
                const ImVec2 g0 = ImGui::GetItemRectMin();
                const ImVec2 g1 = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 c0(g0.x - 6.0f * s, g0.y - 4.0f * s);
                const ImVec2 c1(calloutRight, g1.y + 4.0f * s);
                DashedRect(dl, c0, c1, t.accent, 4.0f * s, 3.0f * s, std::max(1.0f, 1.0f * s));
                // Caption sitting on the top edge of the callout.
                const char* caption = "TARGET DISTANCE";
                const float mic = t.sizeMicro * s;
                const ImVec2 cs = regularFont()->CalcTextSizeA(mic, 1e9f, 0.0f, caption);
                const ImVec2 cp(c0.x + 8.0f * s, c0.y - 0.5f * cs.y);
                dl->AddRectFilled(ImVec2(cp.x - 3.0f * s, cp.y), ImVec2(cp.x + cs.x + 3.0f * s, cp.y + cs.y), t.panel);
                dl->AddText(regularFont(), mic, cp, t.accent, caption);
                ImGui::Unindent(7.0f * s);
                ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
            }

            const Readout items[] = {
                make("MASS \xC2\xB7 KG", d.mass_kg, R.mass, true, "%.3E"),
                make("RADIUS \xC2\xB7 KM", d.radius_km, R.radius, true, "%.1f"),
                make("DIST FROM SUN \xC2\xB7 AU", b.orbit.r_AU, R.distance, true, "%.4f"),
                make("ORBITAL SPEED \xC2\xB7 KM/S", speed, R.speed, false, "%.3f"),
                make("PERIOD \xC2\xB7 DAYS", period, R.period, true, "%.2f"),
                make("ECCENTRICITY", d.elements.e, R.ecc, false, "%.5f"),
                make("INCLINATION \xC2\xB7 DEG", std::fabs(d.elements.i_deg), R.incl, false, "%.3f"),
            };
            readoutGrid(items, 7);
        }
    }
    EndPanel();
}

} // namespace hud
