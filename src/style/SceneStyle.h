#pragma once
// Colours and alphas of the 3D scene, as data. Each HUD theme (hud::HudTheme)
// owns one of these, so switching the HUD theme restyles the scene too. The
// render code reads only this struct: no colour literals in src/render.
//
// Colour budget for the warm theme: ~80% warm neutrals (background, rings,
// grid), ~15% amber structure (ticks, ring labels, core halos), ~5% accent
// (selected orbit, brackets, target label). Alert red is never structural.

#include "style/Palette.h"

namespace style {

struct SceneStyle {
    // --- background: radial lift at the centre, darker at the corners ------------------
    palette::Rgb bgCenter;
    palette::Rgb bgMid;
    palette::Rgb bgEdge;

    // --- reference structure -------------------------------------------------------------
    palette::Rgb gridDim;        // gravity-well grid (secondary to the rings)
    float        gridAlpha;
    palette::Rgb ringDim;        // intermediate rings, spokes
    palette::Rgb ringMid;        // planet rings, fragments, label boxes
    palette::Rgb structure;      // amber: ticks, ring labels, core halo rings, markers
    float        ringAlpha;      // planet (major) rings
    float        ringMinorAlpha; // intermediate rings
    float        tickAlpha;
    float        spokeAlpha;
    float        fragmentAlpha;
    float        markerAlpha;

    // --- Sun core ramp: falloff -> mid -> white-hot ------------------------------------------
    palette::Rgb coreHot;
    palette::Rgb coreMid;
    palette::Rgb coreFalloff;
    float        haloAlpha;      // core halo rings and crown

    // --- highlights ---------------------------------------------------------------------------
    palette::Rgb accent;         // selected orbit, selection brackets, target label (sparingly)
    palette::Rgb alert;          // events/alerts only
    palette::Rgb rimSelected;    // rim light of the selected planet
    palette::Rgb rimPlain;       // rim light of the other planets
    palette::Rgb orbitPlain;     // unselected orbits
    float        orbitPlainAlpha;

    // --- stars: temperature ramp cool -> mid -> warm (desaturated) ------------------------------
    palette::Rgb starCool;
    palette::Rgb starMid;
    palette::Rgb starWarm;

    // --- scene labels (drawn by the HUD overlay) ------------------------------------------------
    palette::Rgb labelLine;      // leader lines
    palette::Rgb labelBorder;    // label box outline
    palette::Rgb labelText;      // label text (the HUD value colour)
    palette::Rgb labelBox;       // label box fill

    // --- colour grade (final composite, scaled by the WARMTH slider) --------------------------
    palette::Rgb gradeShadow;    // shadows are lifted toward this colour
    float        gradeShadowLift; // strength of the lift (0..1 of gradeShadow)
    palette::Rgb gradeHighlight; // highlights are pulled toward this near-white
    float        gradeRedLimit;  // reds more saturated than this are desaturated (0..1)
};

} // namespace style
