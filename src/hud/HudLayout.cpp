#include "hud/HudLayout.h"

#include "hud/Panels.h"
#include "hud/Theme.h"

#include <imgui_internal.h> // DockBuilder API

namespace hud {

void HudLayout::buildDefault(unsigned id) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);

    // Columns. What remains in `centre` becomes the pass-through central node.
    ImGuiID centre = id;
    ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.24f, nullptr, &centre);
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.36f, nullptr, &centre);

    // Left column: status on top, event log at the bottom, controls between.
    ImGuiID leftTop = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.20f, nullptr, &left);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.46f, nullptr, &left);

    // Right column: target on top, history at the bottom, state vector between.
    ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.51f, nullptr, &right);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.53f, nullptr, &right);

    ImGui::DockBuilderDockWindow(kWinStatus, leftTop);
    ImGui::DockBuilderDockWindow(kWinControls, left);
    ImGui::DockBuilderDockWindow(kWinLog, leftBottom);
    ImGui::DockBuilderDockWindow(kWinTarget, rightTop);
    ImGui::DockBuilderDockWindow(kWinTelemetry, right);
    ImGui::DockBuilderDockWindow(kWinHistory, rightBottom);
    ImGui::DockBuilderFinish(id);
}

ViewRect HudLayout::begin() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID id = ImGui::GetID("SolDockspace");
    if (resetRequested_ || ImGui::DockBuilderGetNode(id) == nullptr) {
        buildDefault(id);
        resetRequested_ = false;
    }
    // Tab bars hide themselves for single-window nodes (a small triangle at the
    // node's corner brings them back, to drag a panel elsewhere).
    ImGui::DockSpaceOverViewport(id, vp, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);

    ViewRect r;
    r.min = vp->WorkPos;
    r.max = ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(id)) {
        if (central->Size.x > 1.0f && central->Size.y > 1.0f) {
            r.min = central->Pos;
            r.max = ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y);
        }
    }
    return r;
}

void HudLayout::drawViewportFrame(const ViewRect& r, const char* topLeft, const char* topRight,
                                  const char* bottomLeft, const char* bottomRight) const {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const HudTheme& t = theme();
    const float s = dpi();
    const float inset = 8.0f * s;
    const ImVec2 a(r.min.x + inset, r.min.y + inset);
    const ImVec2 b(r.max.x - inset, r.max.y - inset);
    CornerBrackets(dl, a, b, 18.0f * s, withAlpha(t.accent, 0.85f), 1.5f * s);

    ImFont* f = regularFont();
    const float tiny = t.sizeLabel * s;
    const float pad = 6.0f * s;
    const ImU32 c = t.textLabel;
    auto width = [&](const char* t) { return f->CalcTextSizeA(tiny, 1e9f, 0.0f, t).x; };
    dl->AddText(f, tiny, ImVec2(a.x + pad, a.y + pad), c, topLeft);
    dl->AddText(f, tiny, ImVec2(b.x - pad - width(topRight), a.y + pad), c, topRight);
    dl->AddText(f, tiny, ImVec2(a.x + pad, b.y - pad - tiny), t.textMicro, bottomLeft);
    dl->AddText(f, tiny, ImVec2(b.x - pad - width(bottomRight), b.y - pad - tiny), t.textMicro, bottomRight);

    // Centre reticle.
    const ImVec2 m(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y));
    const ImU32 rc = withAlpha(t.accent, 0.30f);
    dl->AddLine(ImVec2(m.x - 10.0f * s, m.y), ImVec2(m.x - 4.0f * s, m.y), rc, 1.0f);
    dl->AddLine(ImVec2(m.x + 4.0f * s, m.y), ImVec2(m.x + 10.0f * s, m.y), rc, 1.0f);
    dl->AddLine(ImVec2(m.x, m.y - 10.0f * s), ImVec2(m.x, m.y - 4.0f * s), rc, 1.0f);
    dl->AddLine(ImVec2(m.x, m.y + 4.0f * s), ImVec2(m.x, m.y + 10.0f * s), rc, 1.0f);
}

} // namespace hud
