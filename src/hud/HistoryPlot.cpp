#include "hud/HistoryPlot.h"

#include "hud/HudState.h"
#include "hud/Theme.h"
#include "sim/Constants.h"
#include "sim/SolarSystem.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace hud {

namespace {

// Framed placeholder with a centred caption, the same size as the plot.
void emptyPlot(const char* msg, float height) {
    const HudTheme& t = theme();
    const float s = dpi();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), t.block);
    dl->AddRect(p, ImVec2(p.x + w, p.y + height), t.line, 0.0f, 0, 1.0f);
    const float px = t.sizeLabel * s;
    const ImVec2 ts = regularFont()->CalcTextSizeA(px, 1e9f, 0.0f, msg);
    dl->AddText(regularFont(), px, ImVec2(p.x + 0.5f * (w - ts.x), p.y + 0.5f * (height - ts.y)), t.textLabel, msg);
    ImGui::Dummy(ImVec2(w, height));
}

// Dotted line between two pixel positions (horizontal or vertical).
void dotted(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float step) {
    const float len = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y));
    const int n = static_cast<int>(len / step);
    for (int i = 0; i <= n; ++i) {
        const float f = n > 0 ? static_cast<float>(i) / static_cast<float>(n) : 0.0f;
        const ImVec2 p(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f);
        dl->AddRectFilled(p, ImVec2(p.x + 1.0f, p.y + 1.0f), c);
    }
}

// Sparkline of r(t). xs are sample times relative to now (days).
void sparkline(const std::vector<double>& xs, const std::vector<double>& rs, double window, bool backwards,
               float height) {
    const HudTheme& t = theme();
    const float s = dpi();

    // Fit y to the samples ourselves: r(t) often varies by only a few percent,
    // and a shaded fill would otherwise drag auto-fit down to zero.
    double lo = 0.0, hi = 1.0;
    if (!rs.empty()) {
        lo = *std::min_element(rs.begin(), rs.end());
        hi = *std::max_element(rs.begin(), rs.end());
    }
    const double margin = std::max((hi - lo) * 0.10, std::max(std::fabs(hi) * 1e-4, 1e-6));
    lo -= margin;
    hi += margin;
    const double x0 = backwards ? 0.0 : -window;
    const double x1 = backwards ? window : 0.0;

    // 3 y ticks with just enough decimals to tell them apart.
    constexpr int nTicks = 3;
    const double yt0 = lo + 0.1 * (hi - lo);
    const double yt1 = hi - 0.1 * (hi - lo);
    const double step = (yt1 - yt0) / static_cast<double>(nTicks - 1);
    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 0, 4);
    char yFmt[16];
    std::snprintf(yFmt, sizeof yFmt, "%%.%df", decimals);
    constexpr int kXTicks = 5;

    ImPlot::PushStyleColor(ImPlotCol_FrameBg, ImGui::ColorConvertU32ToFloat4(t.block));
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_PlotBorder, ImGui::ColorConvertU32ToFloat4(t.line));
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImGui::ColorConvertU32ToFloat4(withAlpha(t.cool, 0.10f)));
    ImPlot::PushStyleColor(ImPlotCol_AxisText, ImGui::ColorConvertU32ToFloat4(t.textMicro));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(4.0f * s, 4.0f * s));
    pushFontSize(t.sizeMicro);
    if (ImPlot::BeginPlot("##rhist", ImVec2(-1.0f, height), ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
        // A dotted grid is drawn by hand, so ImPlot's solid one is turned off there.
        const ImPlotAxisFlags gridFlags = t.dottedPlotGrid ? ImPlotAxisFlags_NoGridLines : 0;
        ImPlot::SetupAxes(nullptr, nullptr, gridFlags, gridFlags);
        ImPlot::SetupAxisLimits(ImAxis_Y1, lo, hi, ImPlotCond_Always);
        // Newest sample at x = 0; running backwards, the "last N days" lie ahead.
        ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, ImPlotCond_Always);
        // Formats first: ImPlot renders explicit tick labels when the ticks are set.
        ImPlot::SetupAxisFormat(ImAxis_X1, "%+.0f d");
        ImPlot::SetupAxisFormat(ImAxis_Y1, yFmt);
        if (t.dottedPlotGrid) {
            ImPlot::SetupAxisTicks(ImAxis_X1, x0, x1, kXTicks);
        }
        ImPlot::SetupAxisTicks(ImAxis_Y1, yt0, yt1, nTicks);

        if (t.dottedPlotGrid) {
            ImDrawList* pdl = ImPlot::GetPlotDrawList();
            const ImU32 gc = withAlpha(t.textLabel, 0.45f);
            const float dotStep = 4.0f * s;
            ImPlot::PushPlotClipRect();
            for (int i = 0; i < nTicks; ++i) {
                const double y = yt0 + step * static_cast<double>(i);
                dotted(pdl, ImPlot::PlotToPixels(x0, y), ImPlot::PlotToPixels(x1, y), gc, dotStep);
            }
            for (int i = 0; i < kXTicks; ++i) {
                const double x = x0 + (x1 - x0) * static_cast<double>(i) / (kXTicks - 1);
                dotted(pdl, ImPlot::PlotToPixels(x, lo), ImPlot::PlotToPixels(x, hi), gc, dotStep);
            }
            ImPlot::PopPlotClipRect();
        }
        if (!xs.empty()) {
            const int n = static_cast<int>(xs.size());
            if (t.plotFillAlpha > 0.0f) {
                ImPlotSpec fill;
                fill.FillColor = ImGui::ColorConvertU32ToFloat4(t.accent);
                fill.FillAlpha = t.plotFillAlpha;
                ImPlot::PlotShaded("##fill", xs.data(), rs.data(), n, lo, fill);
            }
            ImPlotSpec line;
            line.LineColor = ImGui::ColorConvertU32ToFloat4(t.accent);
            line.LineWeight = (t.dottedPlotGrid ? 1.0f : 1.5f) * s;
            ImPlot::PlotLine("##r", xs.data(), rs.data(), n, line);
        }
        ImPlot::EndPlot();
    }
    popFont();
    ImPlot::PopStyleVar();
    ImPlot::PopStyleColor(5);
}

double phaseOf(const sim::Body& b) {
    const double m = b.orbit.M < 0.0 ? b.orbit.M + sim::kTwoPi : b.orbit.M;
    return m / sim::kTwoPi;
}

// OBSERVATORY: flat gradient bars with a bright top edge.
void phaseHistogram(const sim::SolarSystem& system, int selected, float height) {
    const HudTheme& t = theme();
    const float s = dpi();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = regularFont();
    const float lab = t.sizeLabel * s;
    dl->AddText(f, lab, p, t.textLabel, "ORBIT PHASE  (PERIHELION = 0)");
    const float top = p.y + lab + 3.0f * s;
    const float barMaxH = height - 2.0f * lab - 8.0f * s;
    const float base = top + barMaxH;

    int planets = 0;
    for (int i = 0; i < system.bodyCount(); ++i) {
        planets += system.body(i).parentIndex >= 0 ? 1 : 0;
    }
    const float slot = planets > 0 ? w / static_cast<float>(planets) : w;
    int k = 0;
    for (int i = 0; i < system.bodyCount(); ++i) {
        const sim::Body& b = system.body(i);
        if (b.parentIndex < 0) {
            continue;
        }
        const float frac = static_cast<float>(phaseOf(b));
        const float x0 = p.x + slot * static_cast<float>(k) + 3.0f * s;
        const float x1 = p.x + slot * static_cast<float>(k + 1) - 3.0f * s;
        const bool sel = i == selected;
        const ImU32 barCol = sel ? t.accent : t.data;
        const ImU32 barDim = sel ? t.accentDim : t.dataDim;
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, base), withAlpha(barCol, 0.07f));
        const float barTop = base - barMaxH * frac;
        if (frac > 0.0f) {
            dl->AddRectFilledMultiColor(ImVec2(x0, barTop), ImVec2(x1, base), withAlpha(barCol, 0.85f),
                                        withAlpha(barCol, 0.85f), withAlpha(barDim, 0.25f), withAlpha(barDim, 0.25f));
            dl->AddLine(ImVec2(x0, barTop), ImVec2(x1, barTop), barCol, 1.5f * s);
        }
        const char tag[3] = {b.data().name[0], b.data().name[1], '\0'};
        const float tw = f->CalcTextSizeA(lab, 1e9f, 0.0f, tag).x;
        dl->AddText(f, lab, ImVec2(0.5f * (x0 + x1) - 0.5f * tw, base + 2.0f * s), sel ? t.accent : t.textLabel, tag);
        ++k;
    }
    ImGui::Dummy(ImVec2(w, height));
}

// REACTOR: one tall bar gauge per planet, value above, two-letter label below.
void phaseGauges(const sim::SolarSystem& system, int selected, float height) {
    const float s = dpi();
    int planets = 0;
    for (int i = 0; i < system.bodyCount(); ++i) {
        planets += system.body(i).parentIndex >= 0 ? 1 : 0;
    }
    const float spacing = 5.0f * s;
    const float w = ImGui::GetContentRegionAvail().x;
    const float gw = planets > 0 ? (w - spacing * static_cast<float>(planets - 1)) / static_cast<float>(planets) : w;
    const float gh = std::max(30.0f * s, height); // no caption row: the bars get the full height
    int k = 0;
    for (int i = 0; i < system.bodyCount(); ++i) {
        const sim::Body& b = system.body(i);
        if (b.parentIndex < 0) {
            continue;
        }
        if (k > 0) {
            ImGui::SameLine(0.0f, spacing);
        }
        const float frac = static_cast<float>(phaseOf(b));
        char value[8];
        std::snprintf(value, sizeof value, "%.0f", 100.0 * static_cast<double>(frac));
        const char tag[3] = {b.data().name[0], b.data().name[1], '\0'};
        BarGauge(tag, value, frac, gw, gh, i == selected);
        ++k;
    }
}

} // namespace

DistanceHistory::DistanceHistory() {
    t_.reserve(kCapacity);
    r_.reserve(kCapacity);
    xs_.reserve(kCapacity);
}

void DistanceHistory::reset() {
    t_.clear();
    r_.clear();
}

void DistanceHistory::record(double t_days, double r_AU, double windowDays) {
    const double minStep = windowDays / kMaxSamples;
    if (!t_.empty() && std::fabs(t_days - t_.back()) < minStep) {
        return;
    }
    if (t_.size() >= static_cast<std::size_t>(kCapacity)) {
        // Never grow: drop the oldest sample instead (only reachable on jumpy time).
        t_.erase(t_.begin());
        r_.erase(r_.begin());
    }
    t_.push_back(t_days);
    r_.push_back(r_AU);
    // Drop samples that left the window (in either time direction).
    std::size_t drop = 0;
    while (drop < t_.size() && std::fabs(t_days - t_[drop]) > windowDays) {
        ++drop;
    }
    if (drop > 0) {
        t_.erase(t_.begin(), t_.begin() + static_cast<std::ptrdiff_t>(drop));
        r_.erase(r_.begin(), r_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

void DistanceHistory::draw(const sim::SolarSystem& system, HudState& state, HudEvents& events, double now_days,
                           bool timeRunsBackwards) const {
    const HudTheme& t = theme();
    const float s = dpi();

    // Window selector chips.
    TinyText(t.barGauges ? "r(t) [AU] \xC2\xB7 BARS: ORBIT PHASE %" : "HELIOCENTRIC DISTANCE  r(t)  [AU]");
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine();
        if (ChipToggle(kHistoryWindowLabels[i], i == state.historyWindow) && i != state.historyWindow) {
            state.historyWindow = i;
            events.historyWindowChanged = true;
        }
    }

    // The plot takes whatever the bars leave (the bars are kept compact so r(t)
    // gets most of the panel); the gap is the real item spacing, so the bars end
    // exactly at the panel's content edge instead of being clipped.
    const float histH = (t.barGauges ? 40.0f : 38.0f) * s;
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    const float plotH = std::max(30.0f * s, std::floor(ImGui::GetContentRegionAvail().y - histH - gap));
    const bool hasOrbit = state.selected >= 0 && state.selected < system.bodyCount() &&
                          system.body(state.selected).parentIndex >= 0;
    if (hasOrbit) {
        xs_.resize(t_.size()); // within the reserved capacity: no allocation
        for (std::size_t i = 0; i < t_.size(); ++i) {
            xs_[i] = t_[i] - now_days;
        }
        sparkline(xs_, r_, kHistoryWindowsDays[state.historyWindow], timeRunsBackwards, plotH);
    } else {
        emptyPlot("NO ORBIT \xC2\xB7 HELIOCENTRIC ORIGIN", plotH);
    }
    if (t.barGauges) {
        phaseGauges(system, state.selected, histH);
    } else {
        phaseHistogram(system, state.selected, histH);
    }
}

} // namespace hud
