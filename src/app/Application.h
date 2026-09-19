#pragma once

#include "app/Window.h"
#include "hud/HistoryPlot.h"
#include "hud/HudLayout.h"
#include "hud/HudState.h"
#include "hud/LogPanel.h"
#include "hud/SceneOverlay.h"
#include "render/Camera.h"
#include "render/ScaleMapper.h"
#include "render/SceneRenderer.h"
#include "sim/EventDetector.h"
#include "sim/OrbitProbe.h"
#include "sim/SimClock.h"
#include "sim/SolarSystem.h"

#include <glm/vec3.hpp>
#include <imgui.h>

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

    // Previous-frame state, to log changes whatever caused them (mouse, keys, panels).
    int  prevSelected_ = -1;
    bool prevFollowing_ = false;
    bool prevPaused_ = false;
    float timeDirection_ = 1.0f; // sign of the last non-zero time scale (orbit trails)

    // Dev hooks (environment variables, see README)
    std::string screenshotPath_;
    int         screenshotFrame_ = 90;
    long        frameIndex_ = 0;
};

} // namespace app
