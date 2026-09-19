#include "hud/HudTheme.h"

namespace hud {

namespace {

// ============================================================================
//  REACTOR: warm monochrome on near-black; orange accent, red only for alarms.
// ============================================================================
HudTheme makeReactor() {
    HudTheme t{};
    t.name = "REACTOR";
    t.bg       = toU32(0x0E0605u);
    t.panel    = toU32(0x160908u, 0.94f);
    t.panelTop = toU32(0x160908u, 0.94f);
    t.header   = toU32(0x0F0605u, 0.92f);
    t.block    = toU32(0x1A0B09u, 0.95f); // barely lighter than the panel
    t.rowAlt   = toU32(0x1F0E0Bu, 0.95f); // +2% for alternating rows
    t.line     = toU32(0x4A1C14u);
    t.frame    = toU32(0x0E0605u);

    t.textHero  = toU32(0xFFD6B0u);
    t.textValue = toU32(0xE39A76u);
    t.textLabel = toU32(0x8A4A38u);
    t.textMicro = toU32(0x7A4030u);

    t.accent    = toU32(0xFF6A3Du);
    t.accentDim = toU32(0x8C3317u);
    t.data      = toU32(0xFF6A3Du);
    t.dataDim   = toU32(0x8C3317u);
    t.alarm     = toU32(0xE4281Au);
    t.cool      = toU32(0x7C8CA8u);
    t.chipText  = toU32(0x0E0605u);
    t.corner    = toU32(0xFF6A3Du);
    t.hero      = toU32(0xFF6A3Du);

    // Scene: warm neutrals + amber structure; the orange accent only for the target.
    style::SceneStyle& sc = t.scene;
    sc.bgCenter = 0x14100Du;
    sc.bgMid = 0x0B0908u;
    sc.bgEdge = 0x060504u;
    sc.gridDim = 0x3A2E27u;
    sc.gridAlpha = 0.45f;
    sc.ringDim = 0x3A2E27u;
    sc.ringMid = 0x6A5344u;
    sc.structure = 0xC8955Eu;
    sc.ringAlpha = 0.80f;
    sc.ringMinorAlpha = 0.40f;
    sc.tickAlpha = 0.45f;
    sc.spokeAlpha = 0.40f;
    sc.fragmentAlpha = 0.50f;
    sc.markerAlpha = 0.55f;
    sc.coreHot = 0xFFF1DCu;
    sc.coreMid = 0xFFC27Au;
    sc.coreFalloff = 0xE8894Au;
    sc.haloAlpha = 0.55f;
    sc.accent = 0xFF6A3Du;
    sc.alert = 0xE4281Au;
    sc.rimSelected = 0xC8955Eu;
    sc.rimPlain = 0x6A5344u;
    sc.orbitPlain = 0x6A5344u;
    sc.orbitPlainAlpha = 0.85f;
    sc.starCool = 0xD9DCE2u;
    sc.starMid = 0xEDE3D6u;
    sc.starWarm = 0xF4D6B4u;
    sc.labelLine = 0xC8955Eu;
    sc.labelBorder = 0x6A5344u;
    sc.labelText = 0xE39A76u;
    sc.labelBox = 0x0B0908u;
    sc.gradeShadow = 0x2A1A10u;
    sc.gradeShadowLift = 0.12f;
    sc.gradeHighlight = 0xFFF4E6u;
    sc.gradeRedLimit = 0.55f;

    t.sizeMicro = 10.0f;
    t.sizeLabel = 11.0f;
    t.sizeSmall = 12.0f;
    t.sizeValue = 16.0f;
    t.sizeLarge = 28.0f;
    t.sizeHero = 40.0f;
    t.titleTracking = 3.0f;
    t.scrollbar = 3.0f;
    t.cornerTick = 7.0f;
    t.meterHeight = 2.5f;
    t.scanlineAlpha = 0.035f;
    t.plotFillAlpha = 0.05f;

    t.invertedChips = true;
    t.dashedCallout = true;
    t.barGauges = true;
    t.denseTable = true;
    t.flatControls = true;
    t.heroGlow = true;
    t.gradientPanels = false;
    t.outerGlow = false;
    t.tileBrackets = false;
    t.dottedPlotGrid = true;
    t.logMarkers = true;
    t.statusStrips = true;
    return t;
}

// ============================================================================
//  OBSERVATORY: the original teal/gold look (values from style/Palette.h).
// ============================================================================
HudTheme makeObservatory() {
    HudTheme t{};
    t.name = "OBSERVATORY";
    t.bg       = toU32(palette::SceneBg);
    t.panel    = toU32(palette::PanelBg, palette::PanelAlpha);
    t.panelTop = toU32(palette::PanelTop, palette::PanelAlpha);
    t.header   = toU32(palette::HeaderStrip, 0.85f);
    t.block    = toU32(palette::BlockFill, 0.90f);
    t.rowAlt   = toU32(palette::PanelTop, 0.90f);
    t.line     = toU32(palette::Ui, 0.14f);
    t.frame    = toU32(palette::SceneBg, 0.90f);

    t.textHero  = toU32(palette::TextHero);
    t.textValue = toU32(palette::TextValue);
    t.textLabel = toU32(palette::TextLabel);
    t.textMicro = toU32(palette::TextMicro);

    t.accent    = toU32(palette::Ui);
    t.accentDim = toU32(palette::UiDim);
    t.data      = toU32(palette::World);
    t.dataDim   = toU32(palette::WorldDim);
    t.alarm     = toU32(palette::Warn);
    t.cool      = toU32(palette::Neutral);
    t.chipText  = toU32(palette::SceneBg);
    t.corner    = toU32(palette::Ui, 0.85f);
    t.hero      = toU32(palette::TextHero);

    // Scene: the teal/gold look, with the same structure (rings, ticks, core, grade).
    style::SceneStyle& sc = t.scene;
    sc.bgCenter = 0x0C141Cu;
    sc.bgMid = palette::SceneBg;
    sc.bgEdge = 0x04070Au;
    sc.gridDim = palette::World;
    sc.gridAlpha = 0.60f;
    sc.ringDim = 0x1E3238u;
    sc.ringMid = 0x2F5A60u;
    sc.structure = 0x4FA8A0u;
    sc.ringAlpha = 0.75f;
    sc.ringMinorAlpha = 0.40f;
    sc.tickAlpha = 0.45f;
    sc.spokeAlpha = 0.40f;
    sc.fragmentAlpha = 0.50f;
    sc.markerAlpha = 0.55f;
    sc.coreHot = 0xFFF6E0u;
    sc.coreMid = 0xFFD9A0u;
    sc.coreFalloff = palette::World;
    sc.haloAlpha = 0.45f;
    sc.accent = palette::Ui;
    sc.alert = palette::Warn;
    sc.rimSelected = palette::Ui;
    sc.rimPlain = palette::Neutral;
    sc.orbitPlain = palette::Neutral;
    sc.orbitPlainAlpha = 0.80f;
    sc.starCool = palette::StarCool;
    sc.starMid = palette::StarWhite;
    sc.starWarm = palette::StarWarm;
    sc.labelLine = palette::WorldDim;
    sc.labelBorder = palette::WorldDim;
    sc.labelText = palette::World;
    sc.labelBox = palette::SceneBg;
    sc.gradeShadow = 0x0A1A22u;
    sc.gradeShadowLift = 0.12f;
    sc.gradeHighlight = 0xF0F8FFu;
    sc.gradeRedLimit = 0.80f;

    t.sizeMicro = 10.5f;
    t.sizeLabel = 10.5f;
    t.sizeSmall = 12.0f;
    t.sizeValue = 18.0f;
    t.sizeLarge = 28.0f;
    t.sizeHero = 46.0f;
    t.titleTracking = 0.0f;
    t.scrollbar = 8.0f;
    t.cornerTick = 8.0f;
    t.meterHeight = 3.0f;
    t.scanlineAlpha = 0.0f;
    t.plotFillAlpha = 0.14f;

    t.invertedChips = false;
    t.dashedCallout = false;
    t.barGauges = false;
    t.denseTable = false;
    t.flatControls = false;
    t.heroGlow = false;
    t.gradientPanels = true;
    t.outerGlow = true;
    t.tileBrackets = true;
    t.dottedPlotGrid = false;
    t.logMarkers = false;
    t.statusStrips = false;
    return t;
}

HudTheme g_themes[kThemeCount] = {makeReactor(), makeObservatory()};
ThemeId g_current = ThemeId::Reactor;

} // namespace

const HudTheme& theme() { return g_themes[static_cast<int>(g_current)]; }
const HudTheme& themeById(ThemeId id) { return g_themes[static_cast<int>(id)]; }
ThemeId themeId() { return g_current; }
void setTheme(ThemeId id) { g_current = id; }
const char* themeName(ThemeId id) { return g_themes[static_cast<int>(id)].name; }

void setThemeFonts(ImFont* body, ImFont* large, ImFont* trackedTitle) {
    for (HudTheme& t : g_themes) {
        t.fontBody = body;
        t.fontLarge = large ? large : body;
        t.fontTitle = (t.titleTracking > 0.0f && trackedTitle) ? trackedTitle : body;
    }
}

ImU32 withAlpha(ImU32 c, float alphaMultiplier) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= alphaMultiplier;
    return ImGui::ColorConvertFloat4ToU32(v);
}

} // namespace hud
