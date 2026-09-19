#include "hud/SceneOverlay.h"

#include "hud/Theme.h"

#include <algorithm>
#include <cstddef>

namespace hud {

namespace {

constexpr int kMaxLabelNudges = 6;

bool overlaps(const ImVec4& a, const ImVec4& b) {
    return a.x < b.z && b.x < a.z && a.y < b.w && b.y < a.w; // (minX, minY, maxX, maxY)
}

void drawBrackets(ImDrawList* dl, ImVec2 c, float half, float len, ImU32 col, float thickness) {
    const ImVec2 corners[4] = {{c.x - half, c.y - half}, {c.x + half, c.y - half},
                               {c.x - half, c.y + half}, {c.x + half, c.y + half}};
    const float sx[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    const float sy[4] = {1.0f, 1.0f, -1.0f, -1.0f};
    for (int i = 0; i < 4; ++i) {
        const ImVec2 p = corners[i];
        dl->AddLine(p, ImVec2(p.x + sx[i] * len, p.y), col, thickness);
        dl->AddLine(p, ImVec2(p.x, p.y + sy[i] * len), col, thickness);
    }
}

// A placed label: its leader line and chip.
struct Label {
    ImVec2 start, elbow, end;
    ImVec4 rect;
    const char* name;
    bool selected;
};

} // namespace

void drawSceneOverlay(const SceneOverlayInput& in) {
    if (!in.bodies) {
        return;
    }
    const std::vector<OverlayBody>& bodies = *in.bodies;
    const HudTheme& t = theme();
    const style::SceneStyle& sc = t.scene;
    const ImU32 selCol = toU32(sc.accent);
    const ImU32 lineCol = toU32(sc.labelLine, 0.75f);
    const ImU32 borderCol = toU32(sc.labelBorder);
    const ImU32 textCol = toU32(sc.labelText, 0.95f);
    const ImU32 boxCol = toU32(sc.labelBox, 0.72f);
    const ImU32 structCol = toU32(sc.structure, 0.75f);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * 0.82f;
    const float s = in.dpiScale;
    const float micro = t.sizeMicro * s;
    const ImVec2 pad(3.0f * s, 1.0f * s);

    // Selection brackets (reserved first so labels avoid them). Buffers are reused
    // every frame (capacity is kept), so this draws without allocating.
    static std::vector<ImVec4> placed;
    placed.clear();
    float bracketHalf = 0.0f;
    const int selected = in.selected;
    const bool selVisible = selected >= 0 && static_cast<std::size_t>(selected) < bodies.size() &&
                            bodies[static_cast<std::size_t>(selected)].onScreen;
    if (selVisible) {
        const OverlayBody& b = bodies[static_cast<std::size_t>(selected)];
        bracketHalf = std::max(b.radiusPx + 7.0f * s, 11.0f * s);
        placed.push_back(ImVec4(b.pos.x - bracketHalf, b.pos.y - bracketHalf, b.pos.x + bracketHalf,
                                b.pos.y + bracketHalf));
    }

    // --- place labels (selected first, so it always gets its preferred spot) --------
    static std::vector<Label> labels;
    labels.clear();
    if (in.showLabels) {
        static std::vector<std::size_t> order;
        order.clear();
        if (selVisible) {
            order.push_back(static_cast<std::size_t>(selected));
        }
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (static_cast<int>(i) != selected) {
                order.push_back(i);
            }
        }
        for (std::size_t i : order) {
            const OverlayBody& b = bodies[i];
            if (!b.onScreen) {
                continue;
            }
            const bool isSel = static_cast<int>(i) == selected;
            // Leader: diagonal up-right from the body's edge (outside the brackets
            // for the selection), then a short horizontal run to the tag chip.
            const float r = (isSel ? bracketHalf * 1.4142f : b.radiusPx) + 3.0f * s;
            const ImVec2 start(b.pos.x + r * 0.7071f, b.pos.y - r * 0.7071f);
            ImVec2 elbow(start.x + 12.0f * s, start.y - 12.0f * s);
            const ImVec2 textSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, b.name);
            const float runLen = 8.0f * s;
            auto chipRect = [&](ImVec2 e) {
                const float x0 = e.x + runLen + 4.0f * s - pad.x;
                const float y0 = e.y - textSize.y * 0.5f - pad.y;
                return ImVec4(x0, y0, x0 + textSize.x + 2.0f * pad.x, y0 + textSize.y + 2.0f * pad.y);
            };
            // Nudge upward until the chip no longer overlaps an earlier one.
            ImVec4 rect = chipRect(elbow);
            for (int n = 0; n < kMaxLabelNudges; ++n) {
                bool hit = false;
                for (const ImVec4& p : placed) {
                    hit = hit || overlaps(rect, p);
                }
                if (!hit) {
                    break;
                }
                elbow.y -= textSize.y + 2.0f * pad.y + 2.0f * s;
                elbow.x += 4.0f * s;
                rect = chipRect(elbow);
            }
            placed.push_back(rect);
            labels.push_back({start, elbow, ImVec2(elbow.x + runLen, elbow.y), rect, b.name, isSel});
        }
    }

    // Target data tag: from the brackets' lower-right corner, down-right to a small tag.
    ImVec2 tagStart, tagElbow, tagEnd;
    ImVec4 tagRect(0, 0, 0, 0);
    const bool showTag = selVisible && in.dataTag && *in.dataTag;
    if (showTag) {
        const OverlayBody& b = bodies[static_cast<std::size_t>(selected)];
        const ImVec2 ts = regularFont()->CalcTextSizeA(micro, 1e9f, 0.0f, in.dataTag);
        const float tagW = ts.x + 2.0f * pad.x + 2.0f * s;
        // Down-right by default; mirrored to the lower-left when it would run off the view.
        const float rightEdge = b.pos.x + bracketHalf + 27.0f * s + tagW;
        const float dir = rightEdge > in.viewMaxX ? -1.0f : 1.0f;
        tagStart = ImVec2(b.pos.x + dir * (bracketHalf + 2.0f * s), b.pos.y + bracketHalf + 2.0f * s);
        tagElbow = ImVec2(tagStart.x + dir * 14.0f * s, tagStart.y + 14.0f * s);
        tagEnd = ImVec2(tagElbow.x + dir * 8.0f * s, tagElbow.y);
        const float x0 = dir > 0.0f ? tagEnd.x + 3.0f * s : tagEnd.x - 3.0f * s - tagW;
        const float y0 = tagEnd.y - 0.5f * ts.y - pad.y;
        tagRect = ImVec4(x0, y0, x0 + tagW, y0 + ts.y + 2.0f * pad.y);
    }

    // --- range-ring captions (background texture: dim, drawn under everything) -------
    // They yield to everything else: a caption touching a body label, the
    // brackets, the data tag or an earlier caption (inner rings crowd together
    // when zoomed out) is skipped rather than drawn on top of it.
    if (showTag) {
        placed.push_back(tagRect);
    }
    for (int i = 0; i < in.ringLabelCount; ++i) {
        const OverlayMarker& m = in.ringLabels[i];
        if (!m.visible) {
            continue;
        }
        const ImVec2 at(m.pos.x + 3.0f * s, m.pos.y - micro);
        const ImVec2 ts = regularFont()->CalcTextSizeA(micro, 1e9f, 0.0f, m.text);
        const ImVec4 rect(at.x - 2.0f * s, at.y - 1.0f * s, at.x + ts.x + 2.0f * s, at.y + ts.y + 1.0f * s);
        bool hit = false;
        for (const ImVec4& p : placed) {
            hit = hit || overlaps(rect, p);
        }
        if (!hit) {
            placed.push_back(rect);
            dl->AddText(regularFont(), micro, at, structCol, m.text);
        }
    }

    // --- pass 1: glowing lines (additive) -------------------------------------------------
    if (in.additiveBlend) {
        dl->AddCallback(in.additiveBlend, nullptr);
    }
    for (const Label& l : labels) {
        const ImU32 c = l.selected ? selCol : lineCol;
        // Faint wide halo under a thin line.
        dl->AddLine(l.start, l.elbow, withAlpha(c, 0.18f), 4.0f * s);
        dl->AddLine(l.elbow, l.end, withAlpha(c, 0.18f), 4.0f * s);
        dl->AddLine(l.start, l.elbow, c, 1.0f * s);
        dl->AddLine(l.elbow, l.end, c, 1.0f * s);
    }
    if (selVisible) {
        const OverlayBody& b = bodies[static_cast<std::size_t>(selected)];
        drawBrackets(dl, b.pos, bracketHalf, 5.0f * s, withAlpha(selCol, 0.25f), 4.0f * s);
        drawBrackets(dl, b.pos, bracketHalf, 5.0f * s, selCol, 1.5f * s);
    }
    if (showTag) {
        dl->AddLine(tagStart, tagElbow, withAlpha(selCol, 0.7f), 1.0f * s);
        dl->AddLine(tagElbow, tagEnd, withAlpha(selCol, 0.7f), 1.0f * s);
    }
    // PERI / APO markers on the selected orbit: small diamonds.
    const OverlayMarker* ends[2] = {&in.peri, &in.apo};
    for (const OverlayMarker* m : ends) {
        if (m->visible) {
            const float d = 3.5f * s;
            dl->AddQuad(ImVec2(m->pos.x, m->pos.y - d), ImVec2(m->pos.x + d, m->pos.y), ImVec2(m->pos.x, m->pos.y + d),
                        ImVec2(m->pos.x - d, m->pos.y), selCol, 1.0f * s);
            dl->AddLine(ImVec2(m->pos.x + d, m->pos.y), ImVec2(m->pos.x + d + 5.0f * s, m->pos.y), selCol, 1.0f * s);
        }
    }
    if (in.additiveBlend) {
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    // --- pass 2: chips and text (normal blending, readable over anything) -------------
    for (const Label& l : labels) {
        const ImU32 border = l.selected ? selCol : borderCol;
        const ImU32 text = l.selected ? selCol : textCol;
        dl->AddRectFilled(ImVec2(l.rect.x, l.rect.y), ImVec2(l.rect.z, l.rect.w), boxCol);
        dl->AddRect(ImVec2(l.rect.x, l.rect.y), ImVec2(l.rect.z, l.rect.w), border, 0.0f, 0, 1.0f);
        dl->AddText(font, fontSize, ImVec2(l.rect.x + pad.x, l.rect.y + pad.y), text, l.name);
    }
    for (const OverlayMarker* m : ends) {
        if (m->visible) {
            dl->AddText(regularFont(), micro, ImVec2(m->pos.x + 11.0f * s, m->pos.y - 0.5f * micro), selCol, m->text);
        }
    }
    if (showTag) {
        dl->AddRectFilled(ImVec2(tagRect.x, tagRect.y), ImVec2(tagRect.z, tagRect.w), boxCol);
        dl->AddRect(ImVec2(tagRect.x, tagRect.y), ImVec2(tagRect.z, tagRect.w), withAlpha(selCol, 0.6f), 0.0f, 0,
                    1.0f);
        dl->AddText(regularFont(), micro, ImVec2(tagRect.x + pad.x + 1.0f * s, tagRect.y + pad.y), textCol,
                    in.dataTag);
    }
    if (selVisible && in.following) {
        const OverlayBody& b = bodies[static_cast<std::size_t>(selected)];
        const char* tag = "TRACKING";
        const ImVec2 ts = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, tag);
        dl->AddText(font, fontSize, ImVec2(b.pos.x - ts.x * 0.5f, b.pos.y + bracketHalf + 4.0f * s), selCol, tag);
    }
}

} // namespace hud
