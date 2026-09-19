#include "hud/LogPanel.h"

#include "hud/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace hud {

void EventLog::add(std::string stamp, std::string text, LogKind kind, bool alarm) {
    // The number after the last '=' ("r=0.9833 AU") is highlighted separately.
    const std::size_t eq = text.rfind('=');
    const std::size_t split = eq == std::string::npos ? text.size() : eq + 1;
    entries_.push_back({std::move(stamp), std::move(text), kind, alarm, split});
    while (entries_.size() > kCapacity) {
        entries_.pop_front();
    }
    ++totalAdded_;
}

void EventLog::clear() { entries_.clear(); }

bool EventLog::draw() {
    const HudTheme& t = theme();
    const float s = dpi();
    bool clearPressed = false;

    // Status row: count and CLEAR.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%zu / %zu LINES", entries_.size(), kCapacity);
    TinyText(buf);
    ImGui::SameLine();
    pushFontSize(t.sizeLabel);
    const float buttonW = ImGui::CalcTextSize("CLEAR").x + 2.0f * ImGui::GetStyle().FramePadding.x;
    popFont();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonW));
    if (SmallButton("CLEAR")) {
        clearPressed = true;
    }

    // Height = a whole number of rows (+ padding), so when the list is scrolled to
    // the bottom its top row is fully visible rather than cut in half.
    pushFontSize(t.sizeSmall);
    const float rowSpacing = 1.0f * s;
    const float step = ImGui::GetTextLineHeight() + rowSpacing;
    popFont();
    const float padY = ImGui::GetStyle().WindowPadding.y;
    const float availH = ImGui::GetContentRegionAvail().y;
    const float rows = std::max(1.0f, std::floor((availH - 2.0f * padY + rowSpacing) / step));
    const float childH = rows * step - rowSpacing + 2.0f * padY;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(t.block));
    ImGui::BeginChild("##log", ImVec2(0.0f, childH), ImGuiChildFlags_Borders); // long lines clip at the edge
    // Stick to the bottom only if the view was already at the bottom before new lines arrived.
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

    pushFontSize(t.sizeSmall);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * s, rowSpacing));
    const auto textIn = [](ImU32 c, const char* b, const char* e) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(c));
        ImGui::TextUnformatted(b, e);
        ImGui::PopStyleColor();
    };
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entries_.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Entry& e = entries_[static_cast<std::size_t>(i)];
            const char* text = e.text.c_str();
            if (t.logMarkers) {
                // Small red square for alarm lines (space reserved on every line).
                const float lineH = ImGui::GetTextLineHeight();
                const float m = std::round(5.0f * s);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (e.alarm) {
                    const float y = p.y + 0.5f * (lineH - m);
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + m, y + m), t.alarm);
                }
                ImGui::Dummy(ImVec2(m, lineH));
                ImGui::SameLine();
                // Timestamp dim, event name in the value colour, the number in hero.
                textIn(t.textLabel, e.stamp.c_str(), nullptr);
                ImGui::SameLine();
                const ImU32 nameCol = e.kind == LogKind::System ? t.textLabel : t.textValue;
                textIn(nameCol, text, text + e.split);
                if (e.split < e.text.size()) {
                    ImGui::SameLine(0.0f, 0.0f);
                    textIn(t.textHero, text + e.split, nullptr);
                }
            } else {
                textIn(t.data, e.stamp.c_str(), nullptr);
                ImGui::SameLine();
                const ImU32 c = e.kind == LogKind::Alignment ? t.textHero
                              : e.kind == LogKind::Orbital   ? t.textValue
                                                             : t.textLabel;
                textIn(c, text, nullptr);
            }
        }
    }
    ImGui::PopStyleVar();
    popFont();

    if (totalAdded_ != lastDrawnTotal_ && atBottom) {
        ImGui::SetScrollHereY(1.0f);
    }
    lastDrawnTotal_ = totalAdded_;
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return clearPressed;
}

} // namespace hud
