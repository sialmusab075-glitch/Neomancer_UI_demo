#pragma once

namespace hud {

// View state shared by the HUD panels and the application.
struct HudState {
    int   selected = -1;       // index into SolarSystem::bodies()
    bool  following = false;
    bool  trueScale = false;
    bool  showOrbits = true;
    bool  showGrid = true;
    bool  showStars = true;
    bool  showLabels = true;
    bool  showMoons = false;   // Milestone 4
    bool  bloom = true;        // HDR bloom pass
    bool  finish = true;       // vignette + faint animated noise
    float warmth = 0.6f;       // scene colour-grade strength (0..1)
    bool  showImGuiDemo = false;
    bool  showImPlotDemo = false;
    int   historyWindow = 1;   // index into kHistoryWindowsDays
    int   hudTheme = 0;        // hud::ThemeId (0 = REACTOR, 1 = OBSERVATORY)
    float fps = 0.0f;
    float dpiScale = 1.0f;

    // Earth view: true while it is up (or the camera is flying into it).
    bool  earthView = false;
    // Text for the top strip's NEO DB entry, e.g. "42,666 OBJECTS / 42,819 APPROACHES".
    char  neoStatus[96] = "";
};

constexpr double kHistoryWindowsDays[3] = {90.0, 365.0, 1826.0};
constexpr const char* kHistoryWindowLabels[3] = {"90 D", "1 YR", "5 YR"};

// What the user did in the HUD this frame. The Controls panel changes the
// clock directly; these flags let the application log it and react.
struct HudEvents {
    bool pauseToggled = false;
    bool reversed = false;
    bool stepped = false;
    bool resetToEpoch = false;
    bool timeScaleCommitted = false; // slider released after an edit
    bool selectionChanged = false;
    bool followChanged = false;
    bool scaleModeChanged = false;
    bool resetLayout = false;
    bool historyWindowChanged = false;
    bool clearLog = false;
    bool themeChanged = false;

    // Earth view.
    bool toggleEarthView = false;  // the EARTH VIEW / SOLAR VIEW button (or E)
    bool runQuery = false;         // RUN in NEO FILTER
    bool jumpNextApproach = false; // clock -> the next flyby's closest approach
    bool resultPrev = false;       // select the previous / next result and jump to it
    bool resultNext = false;
    int  rowClicked = -1;          // a NEO RESULTS row was clicked: select it and jump to it
};

} // namespace hud
