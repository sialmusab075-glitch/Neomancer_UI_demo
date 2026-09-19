#pragma once

#include <imgui.h>

#include <vector>

namespace hud {

// One body as seen on screen this frame (window coordinates).
struct OverlayBody {
    const char* name = "";
    ImVec2 pos;
    float  radiusPx = 0.0f;
    bool   onScreen = false;
};

// A projected point with a caption (ring labels, PERI/APO markers).
struct OverlayMarker {
    ImVec2 pos;
    bool   visible = false;
    const char* text = "";
};

struct SceneOverlayInput {
    const std::vector<OverlayBody>* bodies = nullptr;
    int   selected = -1;
    bool  following = false;
    bool  showLabels = true;
    float dpiScale = 1.0f;
    // ImGui draw callback that switches to additive blending (the HUD layer has
    // no GL access of its own); glow lines use it. nullptr = normal blending.
    ImDrawCallback additiveBlend = nullptr;

    const OverlayMarker* ringLabels = nullptr; // range-ring captions ("1.00 AU")
    int ringLabelCount = 0;
    OverlayMarker peri;                        // selected orbit's perihelion / aphelion
    OverlayMarker apo;
    const char* dataTag = nullptr;             // "r 1.0165 AU · v 29.3 km/s" for the target
    float viewMaxX = 1e9f;                     // right edge of the 3D view (the tag flips left past it)
};

// Screen-space decorations drawn over the 3D scene but under every HUD panel:
// planet labels with leader lines, selection brackets and the target data tag,
// PERI/APO markers, and range-ring captions. Colours come from the current
// theme's scene tokens (theme().scene).
void drawSceneOverlay(const SceneOverlayInput& in);

} // namespace hud
