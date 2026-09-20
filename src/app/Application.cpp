#include "app/Application.h"

#include "app/Log.h"
#include "app/Paths.h"
#include "app/Screenshot.h"
#include "hud/Panels.h"
#include "hud/SceneOverlay.h"
#include "hud/HudTheme.h"
#include "hud/Theme.h"
#include "sim/BodyTable.h"
#include "sim/Constants.h"
#include "sim/KeplerSolver.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace app {

namespace {

// ImGui draw callback: additive blending for the glow parts of the HUD overlay.
// The OpenGL3 backend restores its own state on ImDrawCallback_ResetRenderState.
void additiveBlendCallback(const ImDrawList*, const ImDrawCmd*) {
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

constexpr float kDegToRadF = 0.01745329252f;
constexpr float kFollowTransitionSeconds = 0.6f;

struct ViewPreset {
    float distance, minDistance, maxDistance, yaw, pitch;
};

// Default camera for each scale mode: a low angle across the ecliptic.
ViewPreset presetFor(render::ScaleMode mode) {
    if (mode == render::ScaleMode::True) {
        return {620.0f, 0.005f, 2000.0f, 0.65f, 0.34f};
    }
    return {52.0f, 0.3f, 180.0f, 0.65f, 0.30f};
}

float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::string fmt(const char* f, const char* a, double v = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, v);
    return buf;
}

} // namespace

Application::Application() : system_(sim::SolarSystem::createFull()) {
    hud_.selected = system_.indexOfTableRow(sim::kEarth);
}

Application::~Application() { shutdown(); }

int Application::run() {
    logInit(executableDir() + "/solsim.log");
    logInfo("SOL SYSTEM SIM starting; executable dir: %s", executableDir().c_str());

    std::string error;
    if (!init(error)) {
        showFatalError(error);
        shutdown();
        logShutdown();
        return 1;
    }

    double last = glfwGetTime();
    while (!window_.shouldClose()) {
        window_.pollEvents();
        const double now = glfwGetTime();
        const double dt = now - last;
        last = now;

        int fbw = 0, fbh = 0;
        window_.framebufferSize(fbw, fbh);
        if (fbw <= 0 || fbh <= 0) {
            window_.waitEvents(0.1); // minimised: don't spin
            continue;
        }
        const double cpuStart = glfwGetTime();
        frame(dt);
        const double cpuMs = (glfwGetTime() - cpuStart) * 1000.0;
        cpuFrameMs_ = frameIndex_ == 0 ? cpuMs : cpuFrameMs_ * 0.95 + cpuMs * 0.05; // smoothed, swap wait excluded

        ++frameIndex_;
        if (!screenshotPath_.empty() && frameIndex_ >= screenshotFrame_ &&
            (!startInEarth_ || (outcomeFrame_ >= 0 && frameIndex_ >= outcomeFrame_ + screenshotFrame_) ||
             neo_.state() == neo::NeoService::State::Failed) &&
            (!devNeos_ || swarmHavePositions_) && devNeosSelect_ < 0) {
            const bool ok = saveBackBufferBmp(screenshotPath_, fbw, fbh);
            logInfo("screenshot %s: %s", ok ? "written" : "FAILED", screenshotPath_.c_str());
            logSwarmStats();
        logInfo("frame rate at capture: %.1f fps, CPU per frame %.2f ms", static_cast<double>(ImGui::GetIO().Framerate), cpuFrameMs_);
            glfwSetWindowShouldClose(window_.handle(), GLFW_TRUE);
        }
        window_.swapBuffers();
    }

    shutdown();
    logInfo("clean exit");
    logShutdown();
    return 0;
}

bool Application::init(std::string& error) {
    if (!window_.create("SOL SYSTEM SIM", error)) {
        return false;
    }
    if (!scene_.init(error)) {
        return false;
    }
    sceneReady_ = true;
    scene_.rebuild(system_, mapper_);
    resetView();

    initImGui();
    initNeo();
    applyDevHooks();

    sessionId_ = hud::hexId(static_cast<double>(std::time(nullptr)));
    prevSelected_ = hud_.selected;
    prevFollowing_ = hud_.following;
    prevPaused_ = clock_.paused();
    char buf[96];
    std::snprintf(buf, sizeof buf, "SYSTEM ONLINE \xC2\xB7 %d BODIES \xC2\xB7 EPOCH J2000.0", system_.bodyCount());
    logLine(buf, hud::LogKind::System);
    if (hud_.selected >= 0) {
        const sim::BodyData& d = system_.body(hud_.selected).data();
        logLine(std::string("TARGET ") + d.name + " " + d.idTag, hud::LogKind::System);
    }
    if (startInEarth_) {
        hud_.selected = system_.indexOfTableRow(sim::kEarth);
        beginEnterEarth(true);
    }
    return true;
}

void Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    iniPath_ = executableDir() + "/imgui.ini";
    io.IniFilename = iniPath_.c_str();

    hud_.hudTheme = static_cast<int>(hud::ThemeId::Reactor);
    hud::setTheme(hud::ThemeId::Reactor);
    hud::applyTheme(ImGui::GetStyle(), hud::theme());
    baseStyle_ = ImGui::GetStyle();

    loadFonts();

    ImGui_ImplGlfw_InitForOpenGL(window_.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    imguiReady_ = true;

    applyDpiScale(window_.contentScale());
}

void Application::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const std::string fontPath = assetPath("fonts/JetBrainsMono-Regular.ttf");
    if (fileExists(fontPath)) {
        // ImGui 1.92 rasterises glyphs on demand at the final (DPI-scaled)
        // size, so text stays sharp at any content scale.
        ImFont* regular = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 15.0f);
        ImFont* large = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 28.0f);
        fontPath_ = fontPath;
        hud::setThemeFonts(regular, large, nullptr);
        rebuildTitleFont(window_.contentScale());
    } else {
        logError("Font not found: %s (falling back to ImGui's built-in font)", fontPath.c_str());
        ImFont* fallback = io.Fonts->AddFontDefault();
        hud::setThemeFonts(fallback, nullptr, nullptr);
    }
}

// Tracked title font: JetBrains Mono with ImFontConfig::GlyphExtraAdvanceX.
// ImGui 1.92 applies that advance unscaled, so it is set to tracking x DPI and
// the font is rebuilt whenever the content scale changes (between frames).
void Application::rebuildTitleFont(float scale) {
    if (fontPath_.empty()) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (titleFont_) {
        io.Fonts->RemoveFont(titleFont_);
        titleFont_ = nullptr;
    }
    ImFontConfig cfg;
    cfg.GlyphExtraAdvanceX = hud::themeById(hud::ThemeId::Reactor).titleTracking * scale;
    titleFont_ = io.Fonts->AddFontFromFileTTF(fontPath_.c_str(), 12.0f, &cfg);
    titleFontDpi_ = scale;
    hud::setThemeFonts(hud::theme().fontBody, hud::theme().fontLarge, titleFont_);
}

void Application::applyHudTheme(int themeIndex) {
    hud_.hudTheme = themeIndex;
    hud::setTheme(static_cast<hud::ThemeId>(themeIndex));
    hud::applyTheme(baseStyle_, hud::theme());
    applyDpiScale(dpiScale_);
    logLine(std::string("HUD THEME ") + hud::theme().name, hud::LogKind::System);
}

void Application::applyDpiScale(float scale) {
    if (imguiReady_ && std::fabs(scale - titleFontDpi_) > 0.01f) {
        rebuildTitleFont(scale);
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style = baseStyle_;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    dpiScale_ = scale;
    hud_.dpiScale = scale;
    logInfo("DPI content scale %.2f", static_cast<double>(scale));
}

// Environment variables for automated visual checks and demos:
//   SOLSIM_SCREENSHOT=path.bmp  save the frame after SOLSIM_SCREENSHOT_FRAMES (default 90) and exit
//   SOLSIM_TRUE_SCALE=1         start in true-scale mode
//   SOLSIM_SELECT=NAME          select a body (e.g. JUPITER)
//   SOLSIM_TRACK=1              track the selected body
//   SOLSIM_TIME_SCALE=days/s    initial time scale
//   SOLSIM_CAMERA=dist,yawDeg,pitchDeg
//   SOLSIM_THEME=OBSERVATORY       start with the original teal HUD
//   SOLSIM_EARTH=1              start in the Earth view (screenshots wait for the first query result)
//   SOLSIM_NEO_TOPK=n, SOLSIM_NEO_MAXLD=ld   NEO FILTER top-K and max distance for that first query
//   SOLSIM_NEO_PHA=1, SOLSIM_NEO_FROM/TO=YYYY-MM-DD   more of that first query's filter
//   SOLSIM_NEO_SELECT=i, SOLSIM_NEO_HOVER=i|any   select / show as hovered result i (0-based; any = a visible one)
//   SOLSIM_EARTH_ENTER=n, SOLSIM_EARTH_LEAVE=n   enter / leave the Earth view at frame n (with SOLSIM_SELECT=EARTH)
//   SOLSIM_NEOS=pha|1000|5000|20000|all|result   turn the NEOS layer on with that preset
//   SOLSIM_NEOS_LEGEND=distance|pha|diameter|approach, SOLSIM_NEOS_SELECT=i (select the i-th drawn object)
//   SOLSIM_NEOS_DIRECT=n   propagate directly up to n objects (default 2000; 0 = always on the worker)
//   SOLSIM_VSYNC=0              vsync off, for measuring frame cost
//   SOLSIM_NEO_DB=path          the NEO database (default: data/neo.db found above the executable)
void Application::applyDevHooks() {
    screenshotPath_ = envVar("SOLSIM_SCREENSHOT");
    const std::string frames = envVar("SOLSIM_SCREENSHOT_FRAMES");
    if (!frames.empty()) {
        screenshotFrame_ = std::max(1, std::atoi(frames.c_str()));
    }
    if (envVar("SOLSIM_TRUE_SCALE") == "1") {
        hud_.trueScale = true;
        applyScaleMode(true);
    }
    if (envVar("SOLSIM_THEME") == "OBSERVATORY") {
        applyHudTheme(static_cast<int>(hud::ThemeId::Observatory));
    }
    const std::string sel = envVar("SOLSIM_SELECT");
    if (!sel.empty()) {
        const int row = sim::findBody(sel.c_str());
        if (row >= 0) {
            hud_.selected = system_.indexOfTableRow(row);
        }
    }
    const std::string ts = envVar("SOLSIM_TIME_SCALE");
    if (!ts.empty()) {
        clock_.setScale(std::atof(ts.c_str()));
    }
    if (envVar("SOLSIM_EARTH") == "1") {
        startInEarth_ = true;
    }
    const std::string topk = envVar("SOLSIM_NEO_TOPK");
    if (!topk.empty()) {
        earthUi_.filter.topK = std::clamp(std::atoi(topk.c_str()), 1, neo::kMaxTopK);
    }
    const std::string maxld = envVar("SOLSIM_NEO_MAXLD");
    if (!maxld.empty()) {
        earthUi_.filter.maxDistance = static_cast<float>(std::atof(maxld.c_str()));
    }
    if (envVar("SOLSIM_NEO_PHA") == "1") {
        earthUi_.filter.phaOnly = true;
    }
    const std::string from = envVar("SOLSIM_NEO_FROM"), to = envVar("SOLSIM_NEO_TO");
    std::snprintf(earthUi_.filter.dateFrom, sizeof earthUi_.filter.dateFrom, "%s", from.c_str());
    std::snprintf(earthUi_.filter.dateTo, sizeof earthUi_.filter.dateTo, "%s", to.c_str());
    const std::string enterAt = envVar("SOLSIM_EARTH_ENTER"), leaveAt = envVar("SOLSIM_EARTH_LEAVE");
    devEnterFrame_ = enterAt.empty() ? -1 : std::atol(enterAt.c_str());
    devLeaveFrame_ = leaveAt.empty() ? -1 : std::atol(leaveAt.c_str());
    const std::string devSel = envVar("SOLSIM_NEO_SELECT"), devHov = envVar("SOLSIM_NEO_HOVER");
    devSelect_ = devSel.empty() ? -1 : std::atoi(devSel.c_str());
    devHover_ = devHov.empty() ? -1 : (devHov == "any" ? -2 : std::atoi(devHov.c_str()));
    const std::string neos = envVar("SOLSIM_NEOS");
    if (!neos.empty()) {
        devNeos_ = true;
        hud_.showNeos = true;
        hud_.neoPreset = neos == "pha" ? 0 : neos == "1000" ? 1 : neos == "5000" ? 2 : neos == "20000" ? 3
                       : neos == "result" ? 5 : 4; // "all" and anything else
    }
    const std::string neosLegend = envVar("SOLSIM_NEOS_LEGEND");
    if (!neosLegend.empty()) {
        hud_.neoLegend = neosLegend == "pha" ? 1 : neosLegend == "diameter" ? 2 : neosLegend == "approach" ? 3 : 0;
    }
    const std::string neosDirect = envVar("SOLSIM_NEOS_DIRECT");
    if (!neosDirect.empty()) {
        swarmField_.setDirectLimit(static_cast<std::size_t>(std::atol(neosDirect.c_str())));
    }
    const std::string neosSel = envVar("SOLSIM_NEOS_SELECT");
    devNeosSelect_ = neosSel.empty() ? -1 : std::atoi(neosSel.c_str());
    const std::string cam = envVar("SOLSIM_CAMERA");
    if (!cam.empty()) {
        float dist = camera_.distance(), yawDeg = 0.0f, pitchDeg = 0.0f;
        if (std::sscanf(cam.c_str(), "%f,%f,%f", &dist, &yawDeg, &pitchDeg) == 3) {
            camera_.setDistance(dist);
            camera_.setAngles(yawDeg * kDegToRadF, pitchDeg * kDegToRadF);
        }
    }
    if (envVar("SOLSIM_TRACK") == "1") {
        setFollowing(true);
        followBlend_ = 1.0f; // no transition for scripted views
        camera_.setDistance(followDistTo_);
    }
}

void Application::shutdown() {
    if (imguiReady_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
    // GL objects must go before the context.
    if (sceneReady_) {
        scene_.destroy();
        sceneReady_ = false;
    }
    window_.destroy();
}

render::FrameViewport Application::currentViewport(const hud::ViewRect& view) const {
    int fbw = 0, fbh = 0, ww = 0, wh = 0;
    window_.framebufferSize(fbw, fbh);
    glfwGetWindowSize(window_.handle(), &ww, &wh);
    render::FrameViewport vp;
    vp.framebufferPx = glm::vec2(static_cast<float>(std::max(fbw, 1)), static_cast<float>(std::max(fbh, 1)));
    vp.windowPx = glm::vec2(static_cast<float>(std::max(ww, 1)), static_cast<float>(std::max(wh, 1)));
    vp.viewMin = glm::vec2(view.min.x, view.min.y);
    vp.viewSize = glm::vec2(std::max(view.width(), 16.0f), std::max(view.height(), 16.0f));
    vp.dpiScale = dpiScale_;
    return vp;
}

void Application::logLineAt(double t_days, const std::string& text, hud::LogKind kind, bool alarm) {
    log_.add(sim::formatElapsed(t_days - clock_.epochDays()), text, kind, alarm);
}

void Application::logLine(const std::string& text, hud::LogKind kind) { logLineAt(clock_.timeDays(), text, kind); }

void Application::setFollowing(bool follow) {
    hud_.following = follow && hud_.selected >= 0;
    if (!hud_.following) {
        return;
    }
    followBlend_ = 0.0f;
    followFrom_ = camera_.target();
    followDistFrom_ = camera_.distance();
    // Frame the body: close enough to see it, never further out than now.
    const sim::BodyData& d = system_.body(hud_.selected).data();
    const float r = mapper_.bodyRadius(d);
    const float preferred = mapper_.mode == render::ScaleMode::True ? std::max(r * 40.0f, 0.02f)
                                                                     : std::max(r * 14.0f, 2.5f);
    followDistTo_ = std::min(camera_.distance(), preferred);
}

void Application::applyScaleMode(bool trueScale) {
    mapper_.mode = trueScale ? render::ScaleMode::True : render::ScaleMode::Compressed;
    scene_.rebuild(system_, mapper_);
    resetView();
    if (hud_.following) {
        setFollowing(true);
    }
}

void Application::resetView() {
    const ViewPreset p = presetFor(mapper_.mode);
    camera_.setDistanceLimits(p.minDistance, p.maxDistance);
    camera_.setDistance(p.distance);
    camera_.setAngles(p.yaw, p.pitch);
    camera_.setTarget(glm::dvec3(0.0));
}

void Application::frame(double realDt) {
    // Per-monitor DPI: re-scale the HUD when the window moves to another monitor.
    const float scale = window_.contentScale();
    if (std::fabs(scale - dpiScale_) > 0.01f) {
        applyDpiScale(scale);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    updateNeo();
    hud_.earthView = viewMode_ != ViewMode::Solar;

    // Status strips reserve their height, then the dockspace defines the 3D view.
    hud::drawTopStrip(clock_, hud_, sessionId_);
    hud::drawBottomStrip(hud_, log_, hud_.selected >= 0 ? system_.body(hud_.selected).data().name : "NONE");
    const hud::ViewRect viewRect = layout_.begin();
    const render::FrameViewport vp = currentViewport(viewRect);

    // Input (after NewFrame so ImGui's capture flags are current).
    handleInput(vp);
    if (frameIndex_ == devEnterFrame_) {
        requestEarthToggle();
    }
    if (frameIndex_ == devLeaveFrame_ && viewMode_ == ViewMode::Earth) {
        beginLeaveEarth();
    }
    advanceViewMode(realDt);

    // --- simulation step -------------------------------------------------------------
    clock_.update(realDt);
    system_.update(clock_.timeDays());

    const bool earthShown = earthSceneShown();
    if (viewTransitioning() && !earthShown) {
        updateTransitionCamera();
    }
    updateSwarm(earthShown);
    eventBuffer_.clear();
    if (!earthShown) {
        detector_.update(system_, eventBuffer_); // solar events make no sense while the clock plays flybys
    }
    for (const sim::SimEvent& e : eventBuffer_) {
        const bool alignment = e.type == sim::EventType::Opposition || e.type == sim::EventType::Conjunction;
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s %s %s=%.4f AU", system_.body(e.body).data().name, sim::eventName(e.type),
                      alignment ? "d" : "r", e.value_AU);
        // Perihelion and conjunction are the "attention" events (flagged in the log).
        const bool alarm = e.type == sim::EventType::Perihelion || e.type == sim::EventType::Conjunction;
        logLineAt(e.t_days, buf, alignment ? hud::LogKind::Alignment : hud::LogKind::Orbital, alarm);
    }

    if (!earthShown && hud_.selected >= 0) {
        const sim::Body& sel = system_.body(hud_.selected);
        probe_.observe(clock_.timeDays(), sel.orbit.pos_AU);
        if (probe_.hasPeriod() && probe_.crossings() != probeCrossings_) {
            logLineAt(probe_.lastCrossingDays(),
                      fmt("%s REVOLUTION P=%.3f D", sel.data().name, probe_.lastPeriodDays()),
                      hud::LogKind::Orbital);
        }
        probeCrossings_ = probe_.crossings();
        if (sel.parentIndex >= 0) {
            history_.record(clock_.timeDays(), sel.orbit.r_AU, hud::kHistoryWindowsDays[hud_.historyWindow]);
        }
    }

    const render::OrbitCamera& solarCam = viewMode_ == ViewMode::Solar ? camera_ : transCam_;
    if (viewMode_ == ViewMode::Solar) {
        updateFollow(realDt);
    }
    if (!earthShown) {
        visuals_ = scene_.layout(system_, mapper_, solarCam, vp);
    }
    if (viewMode_ == ViewMode::Solar) {
        handlePick(vp);
    } else if (earthShown) {
        earthPicking(vp);
    }

    // --- HUD ---------------------------------------------------------------------------
    hud::HudEvents ev;
    drawHud(ev);
    applyHudEvents(ev, vp);
    logStateChanges();

    // Viewport frame and scene labels, drawn under the panels.
    char tl[96], tr[96], bl[96], br[96];
    std::snprintf(tl, sizeof tl, "VIEW \xC2\xB7 ECLIPTIC J2000 \xC2\xB7 CAM %.1f U", static_cast<double>(solarCam.distance()));
    std::snprintf(tr, sizeof tr, "%s", hud_.trueScale ? "SCALE 1:1 \xC2\xB7 1 AU = 10 U" : "SCALE LOG10 \xC2\xB7 k=10 c=10");
    std::snprintf(bl, sizeof bl, "TGT %s%s", hud_.selected >= 0 ? system_.body(hud_.selected).data().name : "NONE",
                  hud_.following ? " \xC2\xB7 TRACKING" : "");
    char date[32];
    hud::formatDate(date, sizeof date, clock_.timeDays());
    std::snprintf(br, sizeof br, "%+.2f D/S \xC2\xB7 %s", clock_.scale(), date);
    if (earthShown) {
        std::snprintf(tl, sizeof tl, "VIEW \xC2\xB7 EARTH FIXED \xC2\xB7 TILT 23.44\xC2\xB0 \xC2\xB7 CAM %.1f U",
                      static_cast<double>(earthCam_.distance()));
        std::snprintf(tr, sizeof tr, "SCALE LOG10 \xC2\xB7 r = 1 + %.1f\xC2\xB7log10(d/R)",
                      neo::EarthViewScale().logSlope);
        std::snprintf(bl, sizeof bl, "SCHEMATIC DIRECTION \xC2\xB7 REAL DATE, DISTANCE, V_REL");
    }
    layout_.drawViewportFrame(viewRect, tl, tr, bl, br);

    if (earthShown) {
        frameEarthOverlay(vp, viewRect);
    } else {
        frameSolarOverlay(vp, solarCam);
    }
    drawViewFade(viewRect);

    if (hud_.showImGuiDemo) {
        ImGui::ShowDemoWindow(&hud_.showImGuiDemo);
    }
    if (hud_.showImPlotDemo) {
        ImPlot::ShowDemoWindow(&hud_.showImPlotDemo);
    }
    ImGui::Render();

    render::SceneLayers layers;
    layers.orbits = hud_.showOrbits;
    layers.grid = hud_.showGrid;
    layers.stars = hud_.showStars;
    layers.bloom = hud_.bloom;
    layers.finish = hud_.finish;
    layers.warmth = hud_.warmth;
    if (clock_.scale() != 0.0) {
        timeDirection_ = clock_.scale() < 0.0 ? -1.0f : 1.0f;
    }
    if (earthShown) {
        renderEarthScene(vp, layers);
    } else {
        scene_.render(system_, mapper_, solarCam, visuals_, vp, glfwGetTime(), timeDirection_, layers, hud_.selected,
                      hud::theme().scene);
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Application::frameSolarOverlay(const render::FrameViewport& vp, const render::OrbitCamera& cam) {
    std::vector<hud::OverlayBody>& overlay = overlay_;
    overlay.clear(); // capacity is kept between frames
    const float fbToWindow = 1.0f / vp.fbPerWindow();
    for (int i = 0; i < system_.bodyCount(); ++i) {
        const render::BodyVisual& v = visuals_[static_cast<std::size_t>(i)];
        hud::OverlayBody o;
        o.name = system_.body(i).data().name;
        o.pos = ImVec2(v.screen.px.x, v.screen.px.y);
        o.radiusPx = v.radiusPx * fbToWindow;
        o.onScreen = v.screen.onScreen;
        overlay.push_back(o);
    }
    // Range-ring captions, projected from their world positions (no allocation per frame).
    const std::vector<render::RingLabel>& rings = scene_.ringLabels();
    ringMarkers_.resize(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) {
        const glm::dvec3 world(rings[i].pos.x, rings[i].pos.y + scene_.structureY(), rings[i].pos.z);
        const render::ScreenPoint sp = scene_.projectWorld(cam, vp, world);
        ringMarkers_[i].pos = ImVec2(sp.px.x, sp.px.y);
        ringMarkers_[i].visible = sp.onScreen && hud_.showGrid;
        ringMarkers_[i].text = rings[i].text;
    }

    hud::SceneOverlayInput overlayIn;
    overlayIn.bodies = &overlay;
    overlayIn.selected = hud_.selected;
    overlayIn.following = hud_.following;
    overlayIn.showLabels = hud_.showLabels;
    overlayIn.dpiScale = dpiScale_;
    overlayIn.additiveBlend = additiveBlendCallback;
    overlayIn.ringLabels = ringMarkers_.data();
    overlayIn.ringLabelCount = static_cast<int>(ringMarkers_.size());
    if (hud_.selected >= 0 && system_.body(hud_.selected).parentIndex >= 0) {
        // Target: PERI/APO markers on its orbit and a data tag (r, v).
        const sim::Body& b = system_.body(hud_.selected);
        const sim::OrbitalElements& el = b.data().elements;
        const render::ScreenPoint peri =
            scene_.projectWorld(cam, vp, mapper_.toRender(sim::orbitPointAtEccentricAnomaly(el, 0.0)));
        const render::ScreenPoint apo =
            scene_.projectWorld(cam, vp, mapper_.toRender(sim::orbitPointAtEccentricAnomaly(el, sim::kPi)));
        overlayIn.peri = {ImVec2(peri.px.x, peri.px.y), peri.onScreen && hud_.showOrbits, "PERI"};
        overlayIn.apo = {ImVec2(apo.px.x, apo.px.y), apo.onScreen && hud_.showOrbits, "APO"};
        std::snprintf(dataTag_, sizeof dataTag_, "r %.4f AU \xC2\xB7 v %.1f km/s", b.orbit.r_AU,
                      sim::length(b.orbit.vel_kms));
        overlayIn.dataTag = dataTag_;
        overlayIn.viewMaxX = vp.viewMin.x + vp.viewSize.x;
    }
    hud::drawSceneOverlay(overlayIn);
    drawSwarmSelectionOverlay(vp, cam);
}

void Application::drawHud(hud::HudEvents& ev) {
    hud_.fps = ImGui::GetIO().Framerate;
    hud::drawTitleBar(clock_, hud_);
    ev = hud::drawControls(clock_, system_, hud_);
    if (earthSceneShown()) {
        drawEarthPanels(ev);
        return;
    }
    if (selectedRecord_ != neo::kInvalidRecord && neo_.dataset() != nullptr) {
        // An asteroid is selected (from either view): TARGET shows it instead of the planet.
        hud::NeoPanelView view = neoPanelView();
        view.solarView = true;
        view.haveState = true;
        view.objectAu = selectedObjectAu();
        const int earth = system_.indexOfTableRow(sim::kEarth);
        view.earthAu = earth >= 0 ? system_.body(earth).helioPos_AU : sim::Vec3d();
        static const hud::EarthUiState noFlyby;
        hud::drawAsteroidTarget(noFlyby, view);
    } else {
        hud::drawPlanetPanel(system_, hud_, clock_.elapsedDays());
    }
    hud::drawDataGrid(system_, hud_);

    const char* windowTag = hud::kHistoryWindowLabels[hud_.historyWindow];
    if (hud::BeginPanel(hud::kWinHistory, "HISTORY \xC2\xB7 r(t)", windowTag)) {
        history_.draw(system_, hud_, ev, clock_.timeDays(), clock_.scale() < 0.0);
    }
    hud::EndPanel();

    if (hud::BeginPanel(hud::kWinLog, "EVENT LOG", "CH 27 \xC2\xB7 LIVE")) {
        ev.clearLog = log_.draw();
    }
    hud::EndPanel();
}

void Application::applyHudEvents(const hud::HudEvents& ev, const render::FrameViewport& vp) {
    char buf[96];
    applyEarthEvents(ev);
    if (ev.reversed) {
        probe_.reset();
        std::snprintf(buf, sizeof buf, "TIME REVERSED %+.2f D/S", clock_.scale());
        logLine(buf, hud::LogKind::System);
    }
    if (ev.timeScaleCommitted) {
        probe_.reset(); // the scale may have crossed zero
        std::snprintf(buf, sizeof buf, "TIME SCALE %+.2f D/S", clock_.scale());
        logLine(buf, hud::LogKind::System);
    }
    if (ev.stepped) {
        logLine("STEP 1 D (HOLD)", hud::LogKind::System);
    }
    if (ev.resetToEpoch) {
        // A time jump: don't report events "between" the old time and the epoch.
        detector_.reset();
        probe_.reset();
        history_.reset();
        logLine("RESET TO EPOCH J2000.0", hud::LogKind::System);
    }
    if (ev.followChanged) {
        setFollowing(hud_.following);
    }
    if (ev.scaleModeChanged) {
        applyScaleMode(hud_.trueScale);
        updateFollow(0.0);
        visuals_ = scene_.layout(system_, mapper_, camera_, vp);
        logLine(hud_.trueScale ? "DISPLAY SCALE TRUE 1:1" : "DISPLAY SCALE COMPRESSED LOG10", hud::LogKind::System);
    }
    if (ev.historyWindowChanged) {
        history_.reset();
    }
    if (ev.resetLayout) {
        layout_.requestReset();
    }
    if (ev.clearLog) {
        log_.clear();
    }
    if (ev.themeChanged) {
        applyHudTheme(hud_.hudTheme);
    }
}

// Selection, tracking and pause can change from the panels, the mouse or the
// keyboard; logging the difference once per frame covers all of them.
void Application::logStateChanges() {
    if (hud_.selected != prevSelected_) {
        clearSelectedRecord(); // the target combo (or a click) chose a planet
        probe_.reset();
        probeCrossings_ = 0;
        history_.reset();
        if (hud_.following) {
            setFollowing(true); // glide to the new target
        }
        if (hud_.selected >= 0) {
            const sim::BodyData& d = system_.body(hud_.selected).data();
            logLine(std::string("TARGET ") + d.name + " " + d.idTag, hud::LogKind::System);
        }
        prevSelected_ = hud_.selected;
    }
    if (hud_.following != prevFollowing_) {
        logLine(hud_.following && hud_.selected >= 0
                    ? std::string("TRACKING ") + system_.body(hud_.selected).data().name
                    : std::string("TRACKING RELEASED"),
                hud::LogKind::System);
        prevFollowing_ = hud_.following;
    }
    if (clock_.paused() != prevPaused_) {
        logLine(clock_.paused() ? "SIM HOLD" : "SIM RUN", hud::LogKind::System);
        prevPaused_ = clock_.paused();
    }
}

void Application::handleInput(const render::FrameViewport& vp) {
    const ImGuiIO& io = ImGui::GetIO();
    Input& in = window_.input();
    in.beginFrame(window_.handle(), io.WantCaptureMouse, glfwGetTime());

    // Cursor deltas are in window coordinates; convert to framebuffer pixels.
    const float toFb = vp.fbPerWindow();

    if (viewMode_ == ViewMode::Earth) {
        // The Earth stays at the centre: orbit and zoom, no panning.
        if (in.leftDragging()) {
            earthCam_.rotate(in.cursorDx() * toFb / dpiScale_, in.cursorDy() * toFb / dpiScale_);
        }
        if (in.scroll() != 0.0f) {
            earthCam_.zoom(in.scroll());
        }
    } else if (viewMode_ == ViewMode::Solar) {
        if (in.leftDragging()) {
            camera_.rotate(in.cursorDx() * toFb / dpiScale_, in.cursorDy() * toFb / dpiScale_);
        }
        if (in.rightDragging()) {
            hud_.following = false; // panning detaches the camera from its target
            camera_.pan(in.cursorDx() * toFb, in.cursorDy() * toFb, vp.viewSizeFb().y);
        }
        if (in.scroll() != 0.0f) {
            camera_.zoom(in.scroll());
            followDistTo_ = camera_.distance(); // the user's zoom wins over an ongoing transition
            followDistFrom_ = camera_.distance();
        }
    }

    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            clock_.togglePause();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (viewMode_ == ViewMode::Earth) {
                beginLeaveEarth();
            } else {
                hud_.following = false;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            requestEarthToggle();
        }
        if (viewMode_ == ViewMode::Solar && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            setFollowing(!hud_.following);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            applyHudTheme((hud_.hudTheme + 1) % hud::kThemeCount);
        }
        if (viewMode_ == ViewMode::Solar && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            hud_.following = false;
            resetView();
        }
    }
}

void Application::updateFollow(double realDt) {
    if (!hud_.following || hud_.selected < 0) {
        return;
    }
    const glm::dvec3 bodyPos = mapper_.toRender(system_.body(hud_.selected).helioPos_AU);
    if (followBlend_ < 1.0f) {
        followBlend_ = std::min(1.0f, followBlend_ + static_cast<float>(realDt) / kFollowTransitionSeconds);
        const float t = smoothstep01(followBlend_);
        camera_.setTarget(followFrom_ + (bodyPos - followFrom_) * static_cast<double>(t));
        camera_.setDistance(followDistFrom_ + (followDistTo_ - followDistFrom_) * t);
    } else {
        camera_.setTarget(bodyPos);
    }
}

void Application::handlePick(const render::FrameViewport& vp) {
    const Input& in = window_.input();
    if (!in.clicked()) {
        return;
    }
    const int hit = scene_.pick(visuals_, camera_, vp, in.clickX(), in.clickY());
    if (hit < 0) {
        pickSwarmObject(vp, in.clickX(), in.clickY()); // else: clicking empty space keeps the current selection
        return;
    }
    clearSelectedRecord(); // a planet was clicked: it is the target now
    hud_.selected = hit;
    if (in.doubleClicked()) {
        setFollowing(true);
    } else if (hud_.following && hit != prevSelected_) {
        setFollowing(true); // keep tracking, now the new body
    }
}

} // namespace app
