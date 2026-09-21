// The Earth view's half of Application: the view-mode state machine, the NEO
// service hook-up, picking, the overlay and the render call. Everything that
// decides where an asteroid is lives in neo/sim/EarthFlybys (pure and tested);
// this file only wires it to the window, the clock and the HUD.

#include "app/Application.h"

#include "app/Log.h"
#include "app/Paths.h"
#include "hud/Panels.h"
#include "hud/Theme.h"
#include "render/GlColor.h"
#include "sim/BodyTable.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace app {

namespace {

constexpr float kTransitionSeconds = 0.8f; // in and out alike; the swap happens at the midpoint
constexpr double kTwoPi = 6.283185307179586;

float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// data/neo.db beside the sources, found by walking up from the executable (the
// build directory sits inside the repository); SOLSIM_NEO_DB overrides.
std::string findNeoDb() {
    const std::string fromEnv = envVar("SOLSIM_NEO_DB");
    if (!fromEnv.empty()) {
        return fromEnv;
    }
    std::string dir = executableDir();
    for (int level = 0; level < 6 && !dir.empty(); ++level) {
        const std::string candidate = dir + "/data/neo.db";
        if (fileExists(candidate)) {
            return candidate;
        }
        const std::size_t cut = dir.find_last_of("/\\");
        if (cut == std::string::npos) {
            break;
        }
        dir.erase(cut);
    }
    return executableDir() + "/data/neo.db"; // absent: the service reports it as a failed load
}

std::string withCommas(long long n) {
    std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        out += digits[i];
        const std::size_t remaining = digits.size() - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) {
            out += ',';
        }
    }
    return out;
}

ImU32 rgbU32(std::uint32_t hex, float alpha) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, static_cast<int>(alpha * 255.0f + 0.5f));
}

} // namespace

// ---------------------------------------------------------------------------
// Mode and transitions
// ---------------------------------------------------------------------------

bool Application::earthSceneShown() const {
    switch (viewMode_) {
    case ViewMode::Solar: return false;
    case ViewMode::Earth: return true;
    case ViewMode::ToEarth: return swapped_;
    case ViewMode::ToSolar: return !swapped_;
    }
    return false;
}

void Application::initNeo() {
    earthCam_.setDistanceLimits(render::kEarthViewMinDistance, render::kEarthViewMaxDistance);
    earthCam_.setDistance(earthGoalDistance_);
    earthCam_.setAngles(0.55f, 0.30f);
    earthCam_.setTarget(glm::dvec3(0.0));

    const std::string path = findNeoDb();
    logInfo("NEO database: %s", path.c_str());
    neo_.startLoad(path); // a missing file ends in State::Failed with a readable message
}

void Application::requestEarthToggle() {
    if (viewTransitioning()) {
        return;
    }
    if (viewMode_ == ViewMode::Earth) {
        beginLeaveEarth();
        return;
    }
    const int earth = system_.indexOfTableRow(sim::kEarth);
    if (earth >= 0 && hud_.selected == earth) {
        beginEnterEarth(false);
    }
}

void Application::beginEnterEarth(bool instant) {
    if (viewMode_ != ViewMode::Solar) {
        return;
    }
    if (!scene_.earthViewAvailable()) {
        logLine("EARTH VIEW UNAVAILABLE: " + scene_.earthError(), hud::LogKind::System);
        return;
    }
    viewMode_ = ViewMode::ToEarth;
    transition_ = 0.0f;
    swapped_ = false;
    if (instant) {
        commitEnterEarth();
        viewMode_ = ViewMode::Earth;
        transition_ = 1.0f;
    }
}

void Application::beginLeaveEarth() {
    if (viewMode_ != ViewMode::Earth) {
        return;
    }
    earthGoalDistance_ = earthCam_.distance();
    viewMode_ = ViewMode::ToSolar;
    transition_ = 0.0f;
    swapped_ = false;
}

// The world swap at the transition's midpoint, when the screen is dark: the clock
// now plays the flybys, and the solar view's own state is set aside.
void Application::commitEnterEarth() {
    savedFollowing_ = hud_.following;
    savedClockDays_ = clock_.timeDays();
    savedClockScale_ = clock_.scale();
    savedPaused_ = clock_.paused();
    hud_.following = false;
    clock_.setScale(1.0); // one day per second: a typical flyby takes seconds to cross the view
    clock_.setPaused(false);
    earthGoalDistance_ = earthCam_.distance();
    swapped_ = true;
    logLine("EARTH VIEW \xC2\xB7 CLOCK 1 D/S \xC2\xB7 SCHEMATIC FLYBYS", hud::LogKind::System);
    if (haveOutcome_ && outcome_.ok && !outcome_.scene.flybys.empty()) {
        if (earthUi_.selected >= 0) {
            selectFlyby(earthUi_.selected, true); // the asteroid picked in the solar view, if this result has it
        } else if (selectedRecord_ == neo::kInvalidRecord) {
            selectFlyby(0, true);
        }
    }
}

// Back in the solar system exactly as it was left: the same date, rate, pause
// state and tracking, with the detector and probe told about the jump so the log
// does not report every event "between" the two dates.
void Application::commitLeaveEarth() {
    clock_.jumpTo(savedClockDays_);
    clock_.setScale(savedClockScale_);
    clock_.setPaused(savedPaused_);
    hud_.following = savedFollowing_;
    followBlend_ = 1.0f;
    detector_.reset();
    probe_.reset();
    probeCrossings_ = 0;
    history_.reset();
    earthCam_.setDistance(earthGoalDistance_);
    swapped_ = true;
    logLine("SOLAR VIEW \xC2\xB7 CLOCK RESTORED", hud::LogKind::System);
}

void Application::advanceViewMode(double realDt) {
    // The Earth turns in real time, whatever the simulation clock is doing.
    if (earthSceneShown() && !earthUi_.spinPaused) {
        const double dt = std::min(realDt, 0.1);
        spinRadians_ = std::fmod(spinRadians_ + kTwoPi / static_cast<double>(hud::kSpinSecondsPerTurn) *
                                                    static_cast<double>(earthUi_.spinSpeed) * dt,
                                 kTwoPi);
    }
    if (!viewTransitioning()) {
        return;
    }
    transition_ = std::min(1.0f, transition_ + static_cast<float>(realDt) / kTransitionSeconds);
    if (!swapped_ && transition_ >= 0.5f) {
        if (viewMode_ == ViewMode::ToEarth) {
            commitEnterEarth();
        } else {
            commitLeaveEarth();
        }
    }
    // The Earth-side half of the move: the camera flies in from (or out to) a distance.
    // (The solar half is the other one; earthCam_ is not drawn while it runs.)
    const float far = std::min(render::kEarthViewMaxDistance, earthGoalDistance_ * 2.4f);
    if (viewMode_ == ViewMode::ToEarth && swapped_) {
        const float k = smooth01((transition_ - 0.5f) / 0.5f);
        earthCam_.setDistance(far + (earthGoalDistance_ - far) * k);
    } else if (viewMode_ == ViewMode::ToSolar && !swapped_) {
        const float k = smooth01(transition_ / 0.5f);
        earthCam_.setDistance(earthGoalDistance_ + (far - earthGoalDistance_) * k);
    }
    if (transition_ >= 1.0f) {
        viewMode_ = viewMode_ == ViewMode::ToEarth ? ViewMode::Earth : ViewMode::Solar;
        if (viewMode_ == ViewMode::Earth) {
            earthCam_.setDistance(earthGoalDistance_);
        }
    }
}

// The solar camera while it flies to (or back from) the Earth. Computed after the
// bodies have been updated for this frame, so the target is where Earth really is.
void Application::updateTransitionCamera() {
    const int earth = system_.indexOfTableRow(sim::kEarth);
    if (earth < 0) {
        transCam_ = camera_;
        return;
    }
    const glm::dvec3 earthPos = mapper_.toRender(system_.body(earth).helioPos_AU);
    const float r = mapper_.bodyRadius(system_.body(earth).data());
    const float close = mapper_.mode == render::ScaleMode::True ? std::max(r * 40.0f, 0.02f) : std::max(r * 14.0f, 2.5f);
    // e = 0: the user's own view of the solar system; e = 1: close on the Earth.
    const float e = viewMode_ == ViewMode::ToEarth ? smooth01(transition_ / 0.5f)
                                                   : 1.0f - smooth01((transition_ - 0.5f) / 0.5f);
    transCam_ = camera_;
    transCam_.setTarget(camera_.target() + (earthPos - camera_.target()) * static_cast<double>(e));
    transCam_.setDistance(camera_.distance() + (std::min(camera_.distance(), close) - camera_.distance()) * e);
}

void Application::drawViewFade(const hud::ViewRect& viewRect) {
    if (!viewTransitioning()) {
        return;
    }
    // Dark at the midpoint (where the world is swapped), clear at both ends.
    const float a = smooth01(1.0f - std::fabs(1.0f - 2.0f * transition_));
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(viewRect.min.x, viewRect.min.y), ImVec2(viewRect.max.x, viewRect.max.y),
                      rgbU32(hud::theme().scene.bgMid, a));
}

// ---------------------------------------------------------------------------
// The NEO service
// ---------------------------------------------------------------------------

void Application::updateNeo() {
    const neo::NeoService::State state = neo_.state();
    if (state != lastNeoState_) {
        lastNeoState_ = state;
        neoMessage_ = neo_.message();
        switch (state) {
        case neo::NeoService::State::Ready:
            onNeoReady();
            std::snprintf(hud_.neoStatus, sizeof hud_.neoStatus, "%s OBJ \xC2\xB7 %s APP",
                          withCommas(static_cast<long long>(neo_.objectCount())).c_str(),
                          withCommas(static_cast<long long>(neo_.approachCount())).c_str());
            logLine("NEO DB READY \xC2\xB7 " + neoMessage_, hud::LogKind::System);
            break;
        case neo::NeoService::State::Failed:
            std::snprintf(hud_.neoStatus, sizeof hud_.neoStatus, "UNAVAILABLE");
            logLine("NEO DB UNAVAILABLE \xC2\xB7 " + neoMessage_, hud::LogKind::System);
            break;
        case neo::NeoService::State::Loading:
            std::snprintf(hud_.neoStatus, sizeof hud_.neoStatus, "LOADING");
            break;
        case neo::NeoService::State::Idle:
            hud_.neoStatus[0] = '\0';
            break;
        }
    }

    neo::NeoOutcome outcome;
    if (neo_.poll(outcome)) {
        adoptOutcome(std::move(outcome));
    }
    // The first visit runs the default query itself, so the view is never empty.
    if (autoRunPending_ && state == neo::NeoService::State::Ready && viewMode_ != ViewMode::Solar) {
        autoRunPending_ = false;
        submitFilter();
    }
}

void Application::submitFilter() {
    neo::FilterParse parse = neo::toQuery(earthUi_.filter);
    if (!parse.errors.empty()) {
        formErrors_ = std::move(parse.errors);
        return;
    }
    formErrors_.clear();
    submittedSort_ = parse.query.sortBy;
    submittedDirection_ = parse.query.direction;
    neo_.submit(parse.query, neo::EarthViewScale());
}

void Application::adoptOutcome(neo::NeoOutcome&& outcome) {
    outcome_ = std::move(outcome);
    haveOutcome_ = true;
    outcomeFrame_ = frameIndex_;
    neo::resetForNewResult(earthUi_.checks, earthUi_.selected, earthUi_.hovered, outcome_.ok ? outcome_.scene.flybys.size() : 0);
    if (!devChecks_.empty()) { // dev hook, for scripted screenshots: start with some rows unchecked
        const std::size_t n = earthUi_.checks.size();
        const int arg = devChecks_.find(':') != std::string::npos ? std::atoi(devChecks_.c_str() + devChecks_.find(':') + 1) : 0;
        if (devChecks_ == "none") {
            earthUi_.checks.setAll(false);
        } else if (devChecks_.rfind("first:", 0) == 0 && arg > 0) {
            earthUi_.checks.setAll(false);
            earthUi_.checks.setRange(0, static_cast<std::size_t>(arg) - 1, true);
        } else if (devChecks_.rfind("every:", 0) == 0 && arg > 0) {
            for (std::size_t i = 0; i < n; ++i) {
                earthUi_.checks.set(i, i % static_cast<std::size_t>(arg) == 0);
            }
        }
        devChecks_.clear();
    }
    flybyLayout_.clear();
    if (!outcome_.ok) {
        formErrors_ = outcome_.errors;
        scene_.clearFlybyScene();
        return;
    }
    formErrors_.clear();
    refreshPresetSizes();
    if (hud_.neoPreset == static_cast<int>(neo::SwarmPreset::CurrentResult)) {
        swarmDirty_ = true; // the NEOS layer follows the result
    }
    scene_.setFlybyScene(outcome_.scene); // the only place the path meshes are rebuilt
    logLine("NEO QUERY \xC2\xB7 " + outcome_.summary, hud::LogKind::System);
    if (!outcome_.scene.flybys.empty()) {
        // The default query is run on the first visit, so its result usually arrives during the fade,
        // while the SOLAR view is still up: selecting must not jump the clock then (commitEnterEarth
        // saves the solar date, and would save the jumped one). It jumps at the swap instead.
        const bool jump = earthSceneShown();
        selectFlyby(bestFlybyOfFirstObject(), jump);
        if (devSelect_ >= 0) {
            selectFlyby(devSelect_, jump);
        }
    }
}

// The row a learner wants first: the best object's approach that the sort put it
// there for (the closest one for a distance sort, and so on), not merely its
// earliest, which is where the object's approaches happen to start.
int Application::bestFlybyOfFirstObject() const {
    const std::vector<neo::Flyby>& flybys = outcome_.scene.flybys;
    if (flybys.empty()) {
        return -1;
    }
    const bool descending = submittedDirection_ == neo::SortDirection::Descending;
    auto key = [&](const neo::Flyby& f) {
        switch (submittedSort_) {
        case neo::SortField::Distance: return f.distanceAU;
        case neo::SortField::Velocity: return f.vRelKms;
        case neo::SortField::Date: return f.tcaJd;
        default: return 0.0; // an object property: every approach ties, the first stays
        }
    };
    int best = 0;
    for (std::size_t i = 1; i < flybys.size() && flybys[i].object == flybys[0].object; ++i) {
        const double a = key(flybys[i]), b = key(flybys[static_cast<std::size_t>(best)]);
        if (descending ? a > b : a < b) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

void Application::jumpClockToJd(double jd) { clock_.jumpTo(neo::daysSinceJ2000(jd)); }

void Application::selectFlyby(int index, bool jumpClock) {
    if (!haveOutcome_ || !outcome_.ok || index < 0 || static_cast<std::size_t>(index) >= outcome_.scene.flybys.size()) {
        return;
    }
    setSelectedRecord(outcome_.scene.flybys[static_cast<std::size_t>(index)].object, false); // shared with the solar view
    earthUi_.selected = index;
    if (jumpClock) {
        // Land ON the closest approach: the object is at its real distance, at its real date.
        jumpClockToJd(outcome_.scene.flybys[static_cast<std::size_t>(index)].tcaJd);
    }
}

void Application::applyEarthEvents(const hud::HudEvents& ev) {
    if (ev.toggleEarthView) {
        requestEarthToggle();
    }
    if (!earthSceneShown()) {
        return;
    }
    if (ev.clearSelection) {
        clearSelectedRecord(); // its checkbox was unchecked: fall back to no selection
    }
    if (ev.runQuery) {
        submitFilter();
    }
    if (!haveOutcome_ || !outcome_.ok) {
        return;
    }
    const std::vector<neo::Flyby>& flybys = outcome_.scene.flybys;
    if (ev.jumpNextApproach) {
        const int next = neo::nextFlyby(outcome_.scene, earthJd());
        if (next >= 0) {
            selectFlyby(next, true);
        }
    }
    if ((ev.resultPrev || ev.resultNext) && !flybys.empty()) {
        const int n = static_cast<int>(flybys.size());
        const int current = earthUi_.selected;
        int target = 0;
        if (current >= 0) {
            target = (current + (ev.resultNext ? 1 : -1) + n) % n;
        } else if (ev.resultPrev) {
            target = n - 1;
        }
        selectFlyby(target, true);
    }
    if (ev.rowClicked >= 0) {
        selectFlyby(ev.rowClicked, true);
    }
}

// ---------------------------------------------------------------------------
// Picking, HUD, overlay, render
// ---------------------------------------------------------------------------

void Application::earthPicking(const render::FrameViewport& vp) {
    if (haveOutcome_ && outcome_.ok) {
        render::layoutFlybys(outcome_.scene, earthJd(), earthCam_, vp, flybyLayout_, earthUi_.display);
    } else {
        flybyLayout_.clear();
    }
    // (SOLSIM_NEO_HOVER shows a chosen result as hovered until the real cursor is over another.)
    earthUi_.hovered = devHover_ >= 0 && static_cast<std::size_t>(devHover_) < flybyLayout_.size() ? devHover_ : -1;
    if (devHover_ == -2) { // any flyby that is on screen right now, other than the selected one
        for (std::size_t i = 0; i < flybyLayout_.size(); ++i) {
            const render::FlybyScreen& fs = flybyLayout_[i];
            if (fs.visible && !fs.occluded && fs.sp.onScreen && static_cast<int>(i) != earthUi_.selected &&
                earthUi_.checks.drawn(i, earthUi_.selected)) {
                earthUi_.hovered = static_cast<int>(i);
                break;
            }
        }
    }
    if (earthUi_.hovered >= 0 && !earthUi_.checks.drawn(static_cast<std::size_t>(earthUi_.hovered), earthUi_.selected)) {
        earthUi_.hovered = -1; // a hidden flyby is not hovered
    }
    if (viewMode_ != ViewMode::Earth || flybyLayout_.empty() || ImGui::GetIO().WantCaptureMouse) {
        return;
    }
    const Input& in = window_.input();
    const float x = in.cursorX(), y = in.cursorY();
    if (x < vp.viewMin.x || y < vp.viewMin.y || x > vp.viewMin.x + vp.viewSize.x || y > vp.viewMin.y + vp.viewSize.y) {
        return;
    }
    const int under = render::pickFlyby(outcome_.scene, flybyLayout_, vp, x, y, &earthUi_.checks, earthUi_.selected);
    if (under >= 0) {
        earthUi_.hovered = under;
    }
    if (in.clicked()) {
        const float cx = in.clickX(), cy = in.clickY();
        const int hit = render::pickFlyby(outcome_.scene, flybyLayout_, vp, cx, cy, &earthUi_.checks, earthUi_.selected);
        if (hit >= 0) {
            selectFlyby(hit, false); // a click on a marker selects it; the clock stays where it is
        }
    }
}

hud::NeoPanelView Application::neoPanelView() const {
    hud::NeoPanelView v;
    switch (neo_.state()) {
    case neo::NeoService::State::Ready: v.db = hud::NeoPanelView::Db::Ready; break;
    case neo::NeoService::State::Failed: v.db = hud::NeoPanelView::Db::Failed; break;
    default: v.db = hud::NeoPanelView::Db::Loading; break;
    }
    v.dbMessage = neoMessage_.c_str();
    v.dataset = neo_.dataset();
    const bool have = haveOutcome_ && outcome_.ok;
    v.scene = have ? &outcome_.scene : nullptr;
    v.busy = neo_.busy();
    v.summary = have ? outcome_.summary.c_str() : "";
    v.errors = formErrors_.empty() ? nullptr : &formErrors_;
    if (have) {
        v.matchedObjects = outcome_.result.totalObjects;
        v.matchedApproaches = outcome_.result.totalApproaches;
        v.returnedObjects = outcome_.result.rows.size();
        v.queryMs = static_cast<float>(outcome_.totalMs);
    }
    v.jdNow = earthJd();
    v.selectedRecord = selectedRecord_;
    v.textureLoaded = scene_.hasEarthTexture();
    return v;
}

void Application::drawEarthPanels(hud::HudEvents& ev) {
    const hud::NeoPanelView view = neoPanelView();
    hud::drawAsteroidTarget(earthUi_, view);
    hud::drawNeoFilterPanel(earthUi_, view, ev);
    hud::drawEarthViewPanel(earthUi_, view, ev);
    hud::drawNeoResultsPanel(earthUi_, view, ev);
}

void Application::frameEarthOverlay(const render::FrameViewport& vp, const hud::ViewRect&) {
    const neo::EarthViewScale scale;
    const std::vector<neo::ReferenceRing>& rings = neo::referenceRings();
    const std::size_t count = std::min<std::size_t>(rings.size(), 8);
    ringMarkers_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 p = render::ringLabelPoint(rings[i], scale);
        const render::ScreenPoint sp = scene_.projectWorld(earthCam_, vp, glm::dvec3(p));
        std::snprintf(earthRingText_[i], sizeof earthRingText_[i], "%s \xC2\xB7 %s KM", rings[i].label,
                      withCommas(std::llround(rings[i].km)).c_str());
        ringMarkers_[i].pos = ImVec2(sp.px.x, sp.px.y);
        ringMarkers_[i].visible = sp.onScreen && sp.inFront && earthUi_.showRings;
        ringMarkers_[i].text = earthRingText_[i];
    }

    noBodies_.clear();
    hud::SceneOverlayInput in;
    in.bodies = &noBodies_;
    in.showLabels = false;
    in.dpiScale = dpiScale_;
    in.ringLabels = ringMarkers_.data();
    in.ringLabelCount = static_cast<int>(count);
    in.viewMaxX = vp.viewMin.x + vp.viewSize.x;
    hud::drawSceneOverlay(in);

    // Brackets and a data tag on the hovered and the selected flyby.
    if (!haveOutcome_ || !outcome_.ok || flybyLayout_.size() != outcome_.scene.flybys.size()) {
        return;
    }
    const hud::HudTheme& t = hud::theme();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float s = dpiScale_;
    const float pxToWindow = vp.dpiScale / std::max(vp.fbPerWindow(), 1e-3f);
    ImFont* font = hud::regularFont();
    const float textPx = t.sizeLabel * s;

    auto mark = [&](int index, bool selected) {
        if (index < 0 || static_cast<std::size_t>(index) >= flybyLayout_.size()) {
            return;
        }
        const render::FlybyScreen& fs = flybyLayout_[static_cast<std::size_t>(index)];
        if (!fs.visible || !fs.sp.inFront) {
            return;
        }
        const neo::Flyby& f = outcome_.scene.flybys[static_cast<std::size_t>(index)];
        const ImU32 col = f.pha ? t.accent : t.textHero;
        const float r = std::max(f.markerPx * 1.4f, 9.0f) * pxToWindow + 3.0f * s;
        const ImVec2 c(fs.sp.px.x, fs.sp.px.y);
        hud::CornerBrackets(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), r * 0.55f,
                            selected ? col : hud::withAlpha(col, 0.75f), selected ? 1.6f * s : 1.0f * s);

        const neo::Asteroid* a = nullptr;
        if (const neo::Dataset* ds = neo_.dataset()) {
            a = &ds->records()[f.object].object;
        }
        char line1[96], line2[96];
        std::snprintf(line1, sizeof line1, "%s%s", a ? a->label().c_str() : "?", f.pha ? "  PHA" : "");
        std::snprintf(line2, sizeof line2, "%.2f LD \xC2\xB7 %.1f KM/S", f.distanceAU / neo::kLunarDistanceAU, f.vRelKms);
        const float w = std::max(font->CalcTextSizeA(textPx, 1e9f, 0.0f, line1).x,
                                 font->CalcTextSizeA(textPx, 1e9f, 0.0f, line2).x);
        const float pad = 4.0f * s;
        float x = c.x + r + 6.0f * s;
        if (x + w + 2.0f * pad > vp.viewMin.x + vp.viewSize.x) {
            x = c.x - r - 6.0f * s - w - 2.0f * pad; // flip to the left near the view's right edge
        }
        const float y = c.y - textPx - pad;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w + 2.0f * pad, y + 2.0f * textPx + 2.0f * pad),
                          hud::withAlpha(t.bg, 0.78f));
        dl->AddRect(ImVec2(x, y), ImVec2(x + w + 2.0f * pad, y + 2.0f * textPx + 2.0f * pad),
                    hud::withAlpha(col, selected ? 0.9f : 0.5f));
        dl->AddText(font, textPx, ImVec2(x + pad, y + pad), col, line1);
        dl->AddText(font, textPx, ImVec2(x + pad, y + pad + textPx), t.textLabel, line2);
    };
    if (earthUi_.hovered != earthUi_.selected && earthUi_.hovered >= 0 &&
        earthUi_.checks.drawn(static_cast<std::size_t>(earthUi_.hovered), earthUi_.selected)) {
        mark(earthUi_.hovered, false);
    }
    mark(earthUi_.selected, true);
}

void Application::renderEarthScene(const render::FrameViewport& vp, const render::SceneLayers& layers) {
    render::EarthFrame f;
    f.camera = &earthCam_;
    f.viewport = vp;
    f.spinRadians = spinRadians_;
    f.jdNow = earthJd();
    f.scene = haveOutcome_ && outcome_.ok ? &outcome_.scene : nullptr;
    f.layout = &flybyLayout_;
    f.selected = earthUi_.selected;
    f.display = earthUi_.display;
    // A row hovered in NEO RESULTS previews its path only if that flyby is drawn.
    f.hovered = earthUi_.hovered >= 0 && earthUi_.checks.drawn(static_cast<std::size_t>(earthUi_.hovered), earthUi_.selected)
                    ? earthUi_.hovered : -1;
    f.checks = &earthUi_.checks;
    const int earth = system_.indexOfTableRow(sim::kEarth);
    if (earth >= 0) {
        f.earthColour = render::rgb(system_.body(earth).data().colorRGB);
    }
    f.showRings = earthUi_.showRings;
    scene_.renderEarth(f, layers, glfwGetTime(), hud::theme().scene);
}

} // namespace app
