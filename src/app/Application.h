#pragma once

#include "app/Window.h"
#include "hud/EarthUi.h"
#include "hud/HistoryPlot.h"
#include "hud/HudLayout.h"
#include "hud/HudState.h"
#include "hud/LogPanel.h"
#include "hud/SceneOverlay.h"
#include "neo/model/JulianDate.h"
#include "neo/query/NeoService.h"
#include "neo/sim/SwarmField.h"
#include "neo/sim/SwarmLegend.h"
#include "neo/sim/SwarmSelection.h"
#include "render/Camera.h"
#include "render/EarthRenderer.h"
#include "render/ScaleMapper.h"
#include "render/SceneRenderer.h"
#include "sim/EventDetector.h"
#include "sim/OrbitProbe.h"
#include "sim/SimClock.h"
#include "sim/SolarSystem.h"

#include <glm/vec3.hpp>
#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

namespace app {

class Application {
public:
    Application();
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Runs until the window closes. Returns the process exit code.
    int run();

private:
    bool init(std::string& error);
    void shutdown();

    void initImGui();
    void loadFonts();
    void applyDpiScale(float scale);
    void rebuildTitleFont(float scale);
    void applyHudTheme(int themeIndex);
    void applyDevHooks();

    void frame(double realDt);
    render::FrameViewport currentViewport(const hud::ViewRect& view) const;
    void handleInput(const render::FrameViewport& vp);
    void updateFollow(double realDt);
    void handlePick(const render::FrameViewport& vp);
    void drawHud(hud::HudEvents& ev);
    void applyHudEvents(const hud::HudEvents& ev, const render::FrameViewport& vp);
    void logStateChanges();

    // --- Earth view (src/app/EarthView.cpp) ---------------------------------------------
    enum class ViewMode { Solar, ToEarth, Earth, ToSolar };
    bool earthSceneShown() const;             // the Earth scene is what is on screen this frame
    bool viewTransitioning() const { return viewMode_ == ViewMode::ToEarth || viewMode_ == ViewMode::ToSolar; }
    void initNeo();
    void requestEarthToggle();
    void beginEnterEarth(bool instant);
    void beginLeaveEarth();
    void commitEnterEarth();                  // at the midpoint of the transition: swap the world
    void commitLeaveEarth();
    void advanceViewMode(double realDt);
    void updateTransitionCamera();            // the solar camera's flight to / from the Earth
    void updateNeo();                         // status text, result pick-up, automatic first run
    void submitFilter();
    void adoptOutcome(neo::NeoOutcome&& outcome);
    int bestFlybyOfFirstObject() const;
    void selectFlyby(int index, bool jumpClock);
    void jumpClockToJd(double jd);
    void applyEarthEvents(const hud::HudEvents& ev);
    void earthPicking(const render::FrameViewport& vp);
    void drawEarthPanels(hud::HudEvents& ev);
    hud::NeoPanelView neoPanelView() const;
    double earthJd() const { return neo::julianDateFromDaysSinceJ2000(clock_.timeDays()); }
    void frameEarthOverlay(const render::FrameViewport& vp, const hud::ViewRect& viewRect);
    void drawViewFade(const hud::ViewRect& viewRect);
    void renderEarthScene(const render::FrameViewport& vp, const render::SceneLayers& layers);
    void frameSolarOverlay(const render::FrameViewport& vp, const render::OrbitCamera& cam);

    // --- NEOS layer and the shared selection (src/app/SwarmView.cpp) ----------------------------
    void onNeoReady();
    void refreshPresetSizes();
    std::vector<std::uint32_t> currentResultRecords() const;
    void rebuildSwarm();
    void updateSwarmLabel();
    int  slotOf(std::uint32_t record) const;
    void updateSwarm(bool earthShown);
    void setSelectedRecord(std::uint32_t record, bool syncEarth = true);
    void clearSelectedRecord();
    bool pickSwarmObject(const render::FrameViewport& vp, float x, float y);
    sim::Vec3d selectedObjectAu() const;
    void drawSwarmSelectionOverlay(const render::FrameViewport& vp, const render::OrbitCamera& cam);
    void logSwarmStats() const;

    void setFollowing(bool follow);
    void applyScaleMode(bool trueScale);
    void resetView();

    // Log helpers: timestamp = T+ elapsed at `t_days` (default: now).
    void logLine(const std::string& text, hud::LogKind kind);
    void logLineAt(double t_days, const std::string& text, hud::LogKind kind, bool alarm = false);

    Window window_;

    // Simulation (real units)
    sim::SimClock      clock_;
    sim::SolarSystem   system_;
    sim::EventDetector detector_;
    std::vector<sim::SimEvent> eventBuffer_;
    sim::OrbitProbe    probe_; // measures the selected body's period
    int                probeCrossings_ = 0;

    // Rendering (scaled units)
    render::OrbitCamera   camera_;
    render::ScaleMapper   mapper_;
    render::SceneRenderer scene_;
    std::vector<render::BodyVisual> visuals_;

    // Smooth camera transition when tracking starts.
    float      followBlend_ = 1.0f; // 0..1
    glm::dvec3 followFrom_{0.0};
    float      followDistFrom_ = 0.0f;
    float      followDistTo_ = 0.0f;

    // HUD
    hud::HudState        hud_;
    hud::HudLayout       layout_;
    hud::EventLog        log_;
    hud::DistanceHistory history_;
    ImGuiStyle  baseStyle_;     // theme before DPI scaling
    float       dpiScale_ = 0.0f;
    std::string iniPath_;
    std::string fontPath_;
    ImFont*     titleFont_ = nullptr; // tracked instance (GlyphExtraAdvanceX is unscaled: rebuilt per DPI)
    float       titleFontDpi_ = 0.0f;
    unsigned    sessionId_ = 0;
    std::vector<hud::OverlayBody> overlay_;       // reused every frame
    std::vector<hud::OverlayMarker> ringMarkers_; // reused every frame
    char dataTag_[64] = {};
    bool        imguiReady_ = false;
    bool        sceneReady_ = false;

    // Earth view state
    ViewMode          viewMode_ = ViewMode::Solar;
    float             transition_ = 0.0f;      // 0..1 through the current transition
    bool              swapped_ = false;        // the midpoint swap of this transition has happened
    render::OrbitCamera earthCam_;
    render::OrbitCamera transCam_;             // the solar camera while it flies to / from the Earth
    float             earthGoalDistance_ = 24.0f;
    double            spinRadians_ = 0.0;      // Earth rotation: real time, never the sim clock
    neo::NeoService   neo_;
    hud::EarthUiState earthUi_;
    neo::NeoOutcome   outcome_;                // the latest result: rows, flybys, EXPLAIN
    bool              haveOutcome_ = false;
    neo::SortField     submittedSort_ = neo::SortField::None; // the sort of the query the outcome answers
    neo::SortDirection submittedDirection_ = neo::SortDirection::Ascending;
    bool              autoRunPending_ = true;  // run the default query once, on first entry
    std::string       neoMessage_;
    std::vector<std::string> formErrors_;
    std::vector<render::FlybyScreen> flybyLayout_;
    std::vector<hud::OverlayBody> noBodies_;   // the Earth overlay draws ring captions only
    char earthRingText_[8][48] = {};
    neo::NeoService::State lastNeoState_ = neo::NeoService::State::Idle;
    // Saved on entering, restored on leaving: the solar view comes back as it was.
    bool   savedFollowing_ = false;
    double savedClockDays_ = 0.0;
    double savedClockScale_ = 0.0;
    bool   savedPaused_ = false;
    bool   startInEarth_ = false;              // dev hook
    long   outcomeFrame_ = -1;
    int    devSelect_ = -1;                    // dev hooks: a flyby to select / to show as hovered
    int    devHover_ = -1;
    long   devEnterFrame_ = -1;                // dev hooks: enter / leave the Earth view at a frame
    long   devLeaveFrame_ = -1;

    // NEOS layer
    std::unique_ptr<neo::SwarmCatalog> swarmCatalog_;
    neo::SwarmField   swarmField_;
    std::vector<std::uint32_t> swarmRecords_;  // Dataset::records() index of each drawn point
    std::vector<float> swarmPos_;              // what is on screen: 3 floats per point, heliocentric ecliptic AU
    std::vector<float> swarmApproachDays_;
    double            swarmApproachJd_ = -1e30;
    double            swarmUploadT_ = 1e300;
    bool              swarmDirty_ = false;     // the selection must be rebuilt (preset changed / new result)
    bool              swarmHavePositions_ = false;
    int               prevNeoPreset_ = -1;
    int               prevNeoLegend_ = -1;
    bool              prevShowNeos_ = false;
    // The selected asteroid, shared by both views (a Dataset::records() index).
    std::uint32_t     selectedRecord_ = neo::kInvalidRecord;
    int               swarmSlotOfSelected_ = -1;
    neo::SwarmElements selectedElements_;
    sim::OrbitalElements selectedSim_;
    int               devNeosSelect_ = -1;     // dev hook
    bool              devNeos_ = false;

    // Previous-frame state, to log changes whatever caused them (mouse, keys, panels).
    int  prevSelected_ = -1;
    bool prevFollowing_ = false;
    bool prevPaused_ = false;
    float timeDirection_ = 1.0f; // sign of the last non-zero time scale (orbit trails)

    // Dev hooks (environment variables, see README)
    std::string screenshotPath_;
    int         screenshotFrame_ = 90;
    long        frameIndex_ = 0;
    double      cpuFrameMs_ = 0.0; // frame() CPU time, smoothed (the vsync wait in swapBuffers is not in it)
};

} // namespace app
