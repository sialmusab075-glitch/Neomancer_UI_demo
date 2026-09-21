#pragma once

#include "hud/HudState.h"
#include "neo/model/Dataset.h"
#include "neo/query/FilterState.h"
#include "neo/sim/EarthFlybys.h"
#include "sim/Vec3.h"

#include <cstddef>
#include <string>
#include <vector>

namespace hud {

// The Earth view's panels. They live in the same dock nodes as EVENT LOG, STATE
// VECTOR and HISTORY (which are not drawn while the Earth view is up), so the
// layout never changes shape: the right panels simply change.
constexpr const char* kWinFilter  = "NEO FILTER###sol_filter";   // beside CONTROL, under it
constexpr const char* kWinEarth   = "EARTH VIEW###sol_earth";    // where STATE VECTOR is
constexpr const char* kWinResults = "NEO RESULTS###sol_results"; // where HISTORY is

// Interaction state the panels edit and the application owns.
struct EarthUiState {
    neo::NeoFilterState filter;
    float spinSpeed = 1.0f;   // multiples of the default (one turn per kSpinSecondsPerTurn)
    bool  spinPaused = false;
    bool  showRings = true;
    int   selected = -1;      // index into the flyby scene, -1 = none
    int   hovered = -1;
    neo::EarthDisplay display = neo::EarthDisplay::Swarm; // SWARM (default): every flyby at once, animated; PATHS: real dates
    neo::FlybyChecks checks;  // which flybys of the result are drawn (NEO RESULTS checkboxes)
};

// One turn every 20 s at speed 1.0; independent of the simulation clock.
constexpr float kSpinSecondsPerTurn = 20.0f;

// What the panels READ, rebuilt every frame by the application. Nothing here can
// be modified through it, and the panels never reach for the database directly:
// they only see what the query layer produced.
struct NeoPanelView {
    enum class Db { Loading, Ready, Failed };
    Db db = Db::Loading;
    const char* dbMessage = "";

    const neo::Dataset*    dataset = nullptr;   // non-null once the database is Ready
    const neo::FlybyScene* scene = nullptr;     // the latest result's flybys, or null before the first
    bool busy = false;                          // a query is running on the worker

    const char* summary = "";                   // the one-line EXPLAIN of the latest result
    const std::vector<std::string>* errors = nullptr;
    std::size_t matchedObjects = 0;             // before top-K
    std::size_t matchedApproaches = 0;
    std::size_t returnedObjects = 0;

    double jdNow = 0.0;                         // the simulation clock, as a Julian Date

    // The shared selection (either view): a Dataset::records() index, or neo::kInvalidRecord.
    std::uint32_t selectedRecord = neo::kInvalidRecord;
    bool          solarView = false;            // TARGET is drawn for the solar view's NEOS layer
    bool          haveState = false;            // objectAu / earthAu are valid (solar view)
    sim::Vec3d    objectAu;                     // the selected object and the Earth, heliocentric ecliptic AU
    sim::Vec3d    earthAu;
    bool   textureLoaded = false;               // Blue Marble vs the procedural grid
    float  queryMs = 0.0f;
};

void drawNeoFilterPanel(EarthUiState& ui, const NeoPanelView& view, HudEvents& ev);
void drawEarthViewPanel(EarthUiState& ui, const NeoPanelView& view, HudEvents& ev);
void drawNeoResultsPanel(EarthUiState& ui, const NeoPanelView& view, HudEvents& ev);

// TARGET · OBSERVATION for the selected flyby's asteroid.
void drawAsteroidTarget(const EarthUiState& ui, const NeoPanelView& view);

} // namespace hud
