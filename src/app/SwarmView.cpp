// The solar view's NEOS layer, Application's half: which objects are drawn (presets),
// the per-frame propagation and upload, picking, and the selection the two views share.
// Where an asteroid is at time T lives in neo/sim/SwarmField (pure, tested against
// sim::propagate); the point rendering is render/SwarmRenderer.

#include "app/Application.h"

#include "app/Log.h"
#include "app/Paths.h"
#include "hud/Theme.h"
#include "render/GlColor.h"
#include "render/SwarmPick.h"
#include "sim/BodyTable.h"
#include "sim/Constants.h"
#include "sim/KeplerSolver.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace app {

namespace {

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

ImU32 rgbaU32(std::uint32_t hex, float alpha) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, static_cast<int>(alpha * 255.0f + 0.5f));
}

} // namespace

// ---------------------------------------------------------------------------
// Which objects
// ---------------------------------------------------------------------------

// The database just became ready: rank the objects once and tell the menu how big each preset is.
void Application::onNeoReady() {
    const neo::Dataset* ds = neo_.dataset();
    if (ds == nullptr) {
        return;
    }
    swarmCatalog_ = std::make_unique<neo::SwarmCatalog>(*ds);
    refreshPresetSizes();
    hud_.neoAvailable = scene_.swarmAvailable();
    if (!scene_.swarmAvailable()) {
        logLine("NEOS UNAVAILABLE: " + scene_.swarmError(), hud::LogKind::System);
    }
}

void Application::refreshPresetSizes() {
    if (!swarmCatalog_) {
        return;
    }
    for (int i = 0; i < neo::kSwarmPresetCount; ++i) {
        const auto preset = static_cast<neo::SwarmPreset>(i);
        if (preset == neo::SwarmPreset::CurrentResult) {
            hud_.neoPresetSize[i] = static_cast<int>(currentResultRecords().size());
        } else if (preset == neo::SwarmPreset::PhaOnly) {
            hud_.neoPresetSize[i] = static_cast<int>(swarmCatalog_->phaCount());
        } else if (preset == neo::SwarmPreset::All) {
            hud_.neoPresetSize[i] = static_cast<int>(swarmCatalog_->propagatable());
        } else {
            hud_.neoPresetSize[i] = static_cast<int>(std::min(neo::presetCount(preset), swarmCatalog_->propagatable()));
        }
    }
}

std::vector<std::uint32_t> Application::currentResultRecords() const {
    std::vector<std::uint32_t> out;
    if (haveOutcome_ && outcome_.ok) {
        out.reserve(outcome_.result.rows.size());
        for (const neo::ResultRow& row : outcome_.result.rows) {
            out.push_back(row.object);
        }
    }
    return out;
}

// Builds the selection for the current preset and hands it to the propagator and the GPU.
void Application::rebuildSwarm() {
    swarmDirty_ = false;
    const neo::Dataset* ds = neo_.dataset();
    if (!swarmCatalog_ || ds == nullptr) {
        return;
    }
    const auto preset = static_cast<neo::SwarmPreset>(std::clamp(hud_.neoPreset, 0, neo::kSwarmPresetCount - 1));
    const std::vector<std::uint32_t> current = currentResultRecords();
    swarmRecords_ = swarmCatalog_->select(preset, &current);

    const auto start = std::chrono::steady_clock::now();
    swarmField_.setObjects(neo::makeSwarmElements(*ds, swarmRecords_));
    std::vector<neo::SwarmAttr> attrs;
    attrs.reserve(swarmRecords_.size());
    for (const std::uint32_t r : swarmRecords_) {
        attrs.push_back(neo::makeSwarmAttr(ds->records()[r].object));
    }
    scene_.swarm().setObjects(attrs);
    swarmApproachJd_ = -1e30; // the time-to-approach column must be filled for the new set
    swarmHavePositions_ = false;
    swarmUploadT_ = 1e300;
    swarmPos_.clear();
    swarmSlotOfSelected_ = slotOf(selectedRecord_);
    const std::chrono::duration<double, std::milli> ms = std::chrono::steady_clock::now() - start;
    logInfo("NEOS: %zu objects (%s), %s, set up in %.1f ms", swarmRecords_.size(), neo::toString(preset),
            swarmField_.direct() ? "propagated every frame" : "propagated on a worker thread", ms.count());
    updateSwarmLabel();
}

void Application::updateSwarmLabel() {
    if (!swarmCatalog_) {
        hud_.neosLabel[0] = '\0';
        return;
    }
    const std::size_t shown = hud_.showNeos ? swarmRecords_.size()
                                            : static_cast<std::size_t>(hud_.neoPresetSize[std::clamp(hud_.neoPreset, 0, 5)]);
    std::snprintf(hud_.neosLabel, sizeof hud_.neosLabel, "%s / %s", withCommas(static_cast<long long>(shown)).c_str(),
                  withCommas(static_cast<long long>(swarmCatalog_->objectCount())).c_str());
}

int Application::slotOf(std::uint32_t record) const {
    if (record == neo::kInvalidRecord) {
        return -1;
    }
    const auto it = std::find(swarmRecords_.begin(), swarmRecords_.end(), record);
    return it == swarmRecords_.end() ? -1 : static_cast<int>(it - swarmRecords_.begin());
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void Application::updateSwarm(bool earthShown) {
    hud_.neoAvailable = swarmCatalog_ != nullptr && scene_.swarmAvailable();
    // A menu change (or the first time the layer is switched on) rebuilds the selection.
    if (hud_.neoPreset != prevNeoPreset_) {
        prevNeoPreset_ = hud_.neoPreset;
        swarmDirty_ = true;
        if (!hud_.showNeos) {
            updateSwarmLabel();
        }
    }
    if (hud_.showNeos != prevShowNeos_) {
        prevShowNeos_ = hud_.showNeos;
        if (hud_.showNeos && swarmRecords_.empty()) {
            swarmDirty_ = true;
        }
        updateSwarmLabel();
    }
    if (hud_.neoLegend != prevNeoLegend_) {
        prevNeoLegend_ = hud_.neoLegend;
        swarmApproachJd_ = -1e30;
    }
    if (hud_.showNeos && swarmDirty_ && swarmCatalog_) {
        rebuildSwarm();
    }

    render::SwarmDrawParams params;
    const bool active = hud_.showNeos && !earthShown && hud_.neoAvailable && !swarmRecords_.empty();
    if (active) {
        const double t = clock_.timeDays();
        const double rate = clock_.paused() ? 0.0 : clock_.scale();
        if (swarmField_.positionsAt(t, rate, swarmPos_)) {
            if (!swarmHavePositions_ || t != swarmUploadT_) {
                scene_.swarm().updatePositions(swarmPos_.data(), swarmPos_.size() / 3);
                swarmUploadT_ = t;
            }
            swarmHavePositions_ = true;
        }
        // The "time to approach" legend needs each object's next approach; it changes once a day.
        if (hud_.neoLegend == static_cast<int>(neo::SwarmLegend::Approach) && std::fabs(earthJd() - swarmApproachJd_) >= 1.0) {
            swarmApproachJd_ = earthJd();
            const neo::Dataset* ds = neo_.dataset();
            swarmApproachDays_.resize(swarmRecords_.size());
            for (std::size_t i = 0; i < swarmRecords_.size(); ++i) {
                swarmApproachDays_[i] = neo::daysToNextApproach(*ds, swarmRecords_[i], swarmApproachJd_);
            }
            scene_.swarm().updateApproachDays(swarmApproachDays_);
        }
        const hud::HudTheme& th = hud::theme();
        params.enabled = swarmHavePositions_;
        params.legend = static_cast<neo::SwarmLegend>(std::clamp(hud_.neoLegend, 0, neo::kSwarmLegendCount - 1));
        params.colNear = render::rgb(th.scene.accent);
        params.colMid = render::rgb(th.scene.structure);
        params.colFar = render::rgb(th.scene.orbitPlain);
        params.intensity = 1.0f;
        const int earth = system_.indexOfTableRow(sim::kEarth);
        if (earth >= 0) {
            const sim::Vec3d& e = system_.body(earth).helioPos_AU;
            params.earthAu = glm::vec3(static_cast<float>(e.x), static_cast<float>(e.y), static_cast<float>(e.z));
        }
    }
    scene_.setSwarmFrame(params);

    // Dev hook: select the i-th object of the layer once its positions exist.
    if (devNeosSelect_ >= 0 && swarmHavePositions_ && static_cast<std::size_t>(devNeosSelect_) < swarmRecords_.size()) {
        setSelectedRecord(swarmRecords_[static_cast<std::size_t>(devNeosSelect_)]);
        devNeosSelect_ = -1;
    }
}

// ---------------------------------------------------------------------------
// The shared selection
// ---------------------------------------------------------------------------

// `syncEarth` picks the Earth view's flyby for the object; the Earth view itself passes false because it chose its own.
void Application::setSelectedRecord(std::uint32_t record, bool syncEarth) {
    selectedRecord_ = record;
    swarmSlotOfSelected_ = slotOf(record);
    selectedElements_ = neo::SwarmElements();
    selectedSim_ = sim::OrbitalElements();
    if (const neo::Dataset* ds = neo_.dataset(); ds != nullptr && record < ds->records().size()) {
        selectedSim_ = neo::toSimElements(ds->records()[record].object);
        selectedElements_ = neo::makeSwarmElements(selectedSim_);
    }
    // The Earth view's flyby for this object, if its current result has one: the one nearest in time.
    if (!syncEarth) {
        return;
    }
    earthUi_.selected = -1;
    if (haveOutcome_ && outcome_.ok) {
        double best = 1e300;
        const double jd = earthJd();
        for (std::size_t i = 0; i < outcome_.scene.flybys.size(); ++i) {
            const neo::Flyby& f = outcome_.scene.flybys[i];
            if (f.object == record && std::fabs(f.tcaJd - jd) < best) {
                best = std::fabs(f.tcaJd - jd);
                earthUi_.selected = static_cast<int>(i);
            }
        }
    }
}

void Application::clearSelectedRecord() {
    selectedRecord_ = neo::kInvalidRecord;
    swarmSlotOfSelected_ = -1;
    earthUi_.selected = -1;
}

// A click in the solar view that hit no planet: the nearest NEOS point under the cursor.
bool Application::pickSwarmObject(const render::FrameViewport& vp, float x, float y) {
    if (!hud_.showNeos || !swarmHavePositions_ || swarmPos_.empty()) {
        return false;
    }
    const float radius = 10.0f * dpiScale_ / std::max(vp.fbPerWindow(), 1e-3f);
    const int hit = render::pickSwarm(swarmPos_.data(), swarmPos_.size() / 3, mapper_, camera_, vp, x, y, radius);
    if (hit < 0) {
        return false;
    }
    setSelectedRecord(swarmRecords_[static_cast<std::size_t>(hit)]);
    const neo::Dataset* ds = neo_.dataset();
    if (ds != nullptr) {
        logLine("NEO " + ds->records()[selectedRecord_].object.label(), hud::LogKind::System);
    }
    return true;
}

// The object as it is now (one exact propagation), for the TARGET panel and the overlay.
sim::Vec3d Application::selectedObjectAu() const {
    double p[3];
    neo::propagateSwarm(selectedElements_, clock_.timeDays(), p);
    return sim::Vec3d{p[0], p[1], p[2]};
}

// Brackets, a tag and the orbit of the selected asteroid, drawn under the panels.
void Application::drawSwarmSelectionOverlay(const render::FrameViewport& vp, const render::OrbitCamera& cam) {
    const neo::Dataset* ds = neo_.dataset();
    if (selectedRecord_ == neo::kInvalidRecord || ds == nullptr || selectedRecord_ >= ds->records().size()) {
        return;
    }
    const hud::HudTheme& t = hud::theme();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float s = dpiScale_;

    // Its orbit: ninety-six points of the ellipse, mapped like the planets' rings.
    constexpr int kSegments = 96;
    ImVec2 prev;
    bool havePrev = false;
    for (int i = 0; i <= kSegments; ++i) {
        const double eccentric = sim::kTwoPi * static_cast<double>(i) / kSegments;
        const sim::Vec3d ecl = sim::orbitPointAtEccentricAnomaly(selectedSim_, eccentric);
        const render::ScreenPoint sp = scene_.projectWorld(cam, vp, mapper_.toRender(ecl));
        if (sp.inFront && havePrev) {
            dl->AddLine(prev, ImVec2(sp.px.x, sp.px.y), rgbaU32(t.scene.accent, 0.55f), 1.2f * s);
        }
        prev = ImVec2(sp.px.x, sp.px.y);
        havePrev = sp.inFront;
    }

    const render::ScreenPoint sp = scene_.projectWorld(cam, vp, mapper_.toRender(selectedObjectAu()));
    if (!sp.inFront || !sp.onScreen) {
        return;
    }
    const float r = 9.0f * s;
    const ImVec2 c(sp.px.x, sp.px.y);
    hud::CornerBrackets(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), r * 0.55f, t.accent, 1.5f * s);
    const neo::Asteroid& a = ds->records()[selectedRecord_].object;
    char line[96];
    std::snprintf(line, sizeof line, "%s%s", a.label().c_str(),
                  a.classification.isPHA && *a.classification.isPHA ? "  PHA" : "");
    ImFont* font = hud::regularFont();
    const float textPx = t.sizeLabel * s;
    const float w = font->CalcTextSizeA(textPx, 1e9f, 0.0f, line).x;
    const float pad = 4.0f * s;
    float x = c.x + r + 6.0f * s;
    if (x + w + 2.0f * pad > vp.viewMin.x + vp.viewSize.x) {
        x = c.x - r - 6.0f * s - w - 2.0f * pad;
    }
    const float y = c.y - textPx * 0.5f - pad;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w + 2.0f * pad, y + textPx + 2.0f * pad), hud::withAlpha(t.bg, 0.78f));
    dl->AddRect(ImVec2(x, y), ImVec2(x + w + 2.0f * pad, y + textPx + 2.0f * pad), hud::withAlpha(t.accent, 0.8f));
    dl->AddText(font, textPx, ImVec2(x + pad, y + pad), t.textHero, line);
}

void Application::logSwarmStats() const {
    const neo::SwarmField::Stats st = swarmField_.stats();
    logInfo("swarm: %zu objects, %s, grid step %.0f d, %llu nodes, worker %.2f ms/node, direct %.2f ms/frame",
            swarmRecords_.size(), swarmField_.direct() ? "direct" : "nodes", swarmField_.stepDays(),
            static_cast<unsigned long long>(st.nodesComputed), st.lastNodeMs, st.lastDirectMs);
}

} // namespace app
