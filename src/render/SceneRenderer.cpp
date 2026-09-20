#include "render/SceneRenderer.h"

#include "app/Paths.h"
#include "render/GlColor.h"
#include "render/Starfield.h"
#include "sim/BodyTable.h"
#include "sim/SolarSystem.h"

#include <glad/gl.h>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace render {

namespace {

constexpr int kStarCount = 5000;
constexpr int kSunParticles = 4200;
constexpr float kSunParticleScale = 1.12f;
constexpr float kSunParticleModePx = 10.0f; // below this on-screen radius the Sun is a plain sphere

// Depth fade: full strength near the camera target, fading to kFadeMin far behind it.
constexpr float kFadeNearFactor = 0.55f; // x camera distance
constexpr float kFadeFarFactor = 1.9f;   // x camera distance (+ part of the system extent)
constexpr float kFadeMin = 0.22f;

// Sun core: halo ring radii (x Sun radius), pulse period and depth, crown radius and spin.
constexpr float kHaloRadii[3] = {1.30f, 1.55f, 1.85f};
constexpr float kHaloPulsePeriod = 6.0f; // seconds
constexpr float kHaloPulseDepth = 0.10f; // +/- 10% radius and alpha
constexpr float kCrownRadius = 2.15f;
constexpr float kCrownSpin = 0.08f;      // rad/s

// Point modes in points.vert.
constexpr float kModeStars = 0.0f;
constexpr float kModeSun = 1.0f;
constexpr float kModeTint = 2.0f;

GridParams gridParamsFor(ScaleMode mode) {
    GridParams p;
    if (mode == ScaleMode::True) {
        p.ringSpacing = 5.0f; // 0.5 AU
        p.ringCount = 64;
        p.wellK = 300.0f;
        p.wellSoft = 20.0f;
        p.yOffset = -0.05f;
        p.dipK = 2.5f;
        p.dipWidth = 5.0f;
    } else {
        p.ringSpacing = 1.25f;
        p.ringCount = 22;
        p.wellK = 8.4f;
        p.wellSoft = 2.2f;
        p.yOffset = -0.03f;
        p.dipK = 0.5f;
        p.dipWidth = 0.9f;
    }
    return p;
}

// Rotation that turns the XY plane to face the camera (inverse of the view rotation).
glm::mat4 billboard(const glm::mat4& view) {
    const glm::mat3 r = glm::transpose(glm::mat3(view));
    return glm::mat4(r);
}

} // namespace

bool SceneRenderer::init(std::string& error) {
    const auto path = [](const char* name) { return app::assetPath(std::string("shaders/") + name); };
    if (!bodyShader_.loadFromFiles(path("body.vert"), path("body.frag"), error)) return false;
    if (!orbitShader_.loadFromFiles(path("orbit.vert"), path("orbit.frag"), error, path("orbit.geom"))) return false;
    if (!lineShader_.loadFromFiles(path("line.vert"), path("line.frag"), error)) return false;
    if (!pointShader_.loadFromFiles(path("points.vert"), path("points.frag"), error)) return false;
    if (!structShader_.loadFromFiles(path("struct.vert"), path("line.frag"), error)) return false;
    if (!backgroundShader_.loadFromFiles(path("fullscreen.vert"), path("background.frag"), error)) return false;
    if (!post_.init(error)) return false;
    glGenVertexArrays(1, &emptyVao_);

    sphere_.create();
    stars_.create(generateStarfield(kStarCount, 0x5017u));
    sunParticles_.create(generateSunParticles(kSunParticles, 0x50u));
    haloCircle_.create(generateUnitCircle(128));
    crown_.create(generateCrown(72));

    // The Earth view is optional: if its shaders fail, the solar view still works.
    earthReady_ = earth_.init(earthError_);
    return true;
}

void SceneRenderer::destroy() {
    earth_.destroy();
    post_.destroy();
    orbits_.destroy();
    grid_.destroy();
    stars_.destroy();
    sunParticles_.destroy();
    majorRings_.destroy();
    minorRings_.destroy();
    ticks_.destroy();
    spokes_.destroy();
    fragments_.destroy();
    markers_.destroy();
    haloCircle_.destroy();
    crown_.destroy();
    sphere_.destroy();
    if (emptyVao_) glDeleteVertexArrays(1, &emptyVao_);
    emptyVao_ = 0;
    bodyShader_.destroy();
    orbitShader_.destroy();
    lineShader_.destroy();
    pointShader_.destroy();
    structShader_.destroy();
    backgroundShader_.destroy();
}

void SceneRenderer::rebuild(const sim::SolarSystem& system, const ScaleMapper& mapper) {
    orbits_.build(system, mapper);
    gridParams_ = gridParamsFor(mapper.mode);
    grid_.create(generateEclipticGrid(gridParams_));

    // Range rings at every planet's semi-major axis (mapped), out to past the last one.
    StructureParams sp;
    float sunR = 0.0f;
    for (const sim::Body& b : system.bodies()) {
        const sim::BodyData& d = b.data();
        if (d.kind == sim::BodyKind::Star) {
            sunR = mapper.bodyRadius(d);
        } else if (b.parentIndex >= 0) {
            sp.ringRadii.push_back(static_cast<float>(mapper.distanceToRender(d.elements.a_AU)));
            sp.ringAU.push_back(d.elements.a_AU);
        }
    }
    const float last = sp.ringRadii.empty() ? 10.0f : sp.ringRadii.back();
    sp.innerRadius = std::max(sunR * 2.4f, 0.02f * last);
    sp.outerRadius = last * 1.12f;
    const StructureGeometry g = generateStructure(sp);
    majorRings_.create(g.majorRings);
    minorRings_.create(g.minorRings);
    ticks_.create(g.ticks);
    spokes_.create(g.spokes);
    fragments_.create(g.fragments);
    markers_.create(g.markers);
    structureLabels_ = g.labels;
    // Just below the orbital plane (orbits stay on top) and above the grid.
    structureY_ = 0.5f * gridParams_.yOffset;
}

std::vector<BodyVisual> SceneRenderer::layout(const sim::SolarSystem& system, const ScaleMapper& mapper,
                                              const OrbitCamera& camera, const FrameViewport& vp) const {
    const float aspect = vp.aspect();
    const glm::mat4 viewProj = camera.projection(aspect) * camera.view();
    const glm::dvec3 target = camera.target();
    const glm::vec3 eye = camera.eyeOffset();

    std::vector<BodyVisual> out;
    out.reserve(system.bodies().size());
    for (const sim::Body& body : system.bodies()) {
        const sim::BodyData& data = body.data();
        BodyVisual v;
        // Subtract in double, then narrow: precision stays near the target.
        v.rel = glm::vec3(mapper.toRender(body.helioPos_AU) - target);
        const float depth = std::max(glm::length(v.rel - eye), 1e-6f);
        const float wpp = camera.worldPerPixel(depth, vp.viewSizeFb().y);
        const float minPx = (data.kind == sim::BodyKind::Star ? kMinSunPx : kMinBodyPx) * vp.dpiScale;
        v.radius = std::max(mapper.bodyRadius(data), minPx * wpp);
        v.pickRadius = std::max(v.radius, kMinPickPx * vp.dpiScale * wpp);
        v.radiusPx = v.radius / wpp;
        v.screen = projectToScreen(viewProj, v.rel, vp.viewSize.x, vp.viewSize.y);
        v.screen.px += vp.viewMin;
        out.push_back(v);
    }
    return out;
}

ScreenPoint SceneRenderer::projectWorld(const OrbitCamera& camera, const FrameViewport& vp,
                                        const glm::dvec3& world) const {
    const glm::mat4 viewProj = camera.projection(vp.aspect()) * camera.view();
    ScreenPoint sp = projectToScreen(viewProj, glm::vec3(world - camera.target()), vp.viewSize.x, vp.viewSize.y);
    sp.px += vp.viewMin;
    return sp;
}

int SceneRenderer::pick(const std::vector<BodyVisual>& visuals, const OrbitCamera& camera,
                        const FrameViewport& vp, float windowX, float windowY) const {
    const float aspect = vp.aspect();
    const glm::mat4 viewProj = camera.projection(aspect) * camera.view();
    const Ray ray = makePickRay(viewProj, windowX - vp.viewMin.x, windowY - vp.viewMin.y, vp.viewSize.x,
                                vp.viewSize.y);

    int best = -1;
    float bestT = 0.0f;
    for (std::size_t i = 0; i < visuals.size(); ++i) {
        const float t = raySphere(ray, visuals[i].rel, visuals[i].pickRadius);
        if (t >= 0.0f && (best < 0 || t < bestT)) {
            best = static_cast<int>(i);
            bestT = t;
        }
    }
    return best;
}

void SceneRenderer::drawBackground(const FrameViewport& vp, const style::SceneStyle& st) const {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    backgroundShader_.use();
    backgroundShader_.set("uCenter", rgb(st.bgCenter));
    backgroundShader_.set("uMid", rgb(st.bgMid));
    backgroundShader_.set("uEdge", rgb(st.bgEdge));
    backgroundShader_.set("uResolution", vp.viewSizeFb());
    glBindVertexArray(emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
}

void SceneRenderer::render(const sim::SolarSystem& system, const ScaleMapper& mapper, const OrbitCamera& camera,
                           const std::vector<BodyVisual>& visuals, const FrameViewport& vp, double timeSeconds,
                           float timeDirection, const SceneLayers& layers, int selectedBody,
                           const style::SceneStyle& st) {
    // Whole window: base colour (panels are drawn over it by ImGui).
    const glm::vec3 bg = rgb(st.bgMid);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(vp.framebufferPx.x), static_cast<GLsizei>(vp.framebufferPx.y));
    glClearColor(bg.r, bg.g, bg.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // View rectangle in framebuffer pixels (GL origin is bottom-left).
    const float k = vp.fbPerWindow();
    const glm::vec2 viewFb = vp.viewSizeFb();
    const int w = std::max(1, static_cast<int>(viewFb.x));
    const int h = std::max(1, static_cast<int>(viewFb.y));
    const int x = static_cast<int>(vp.viewMin.x * k);
    const int y = static_cast<int>(vp.framebufferPx.y - (vp.viewMin.y + vp.viewSize.y) * k);
    const glm::vec2 targetPx(static_cast<float>(w), static_cast<float>(h));

    if (post_.begin(w, h)) {
        // Offscreen HDR target: background, scene, then resolve + bloom + grade + composite.
        glClear(GL_DEPTH_BUFFER_BIT);
        drawBackground(vp, st);
        drawScene(system, mapper, camera, visuals, vp, targetPx, timeSeconds, timeDirection, layers, selectedBody, st);
        PostSettings ps;
        ps.bloom = layers.bloom;
        ps.finish = layers.finish;
        ps.warmth = layers.warmth;
        ps.shadowLift = rgb(st.gradeShadow) * st.gradeShadowLift;
        ps.highlight = rgb(st.gradeHighlight);
        ps.redLimit = st.gradeRedLimit;
        post_.end(ps, x, y, static_cast<float>(std::fmod(timeSeconds, 1000.0)));
    } else {
        // Fallback: straight into the default framebuffer, no bloom/finish/grade.
        glViewport(x, y, w, h);
        drawBackground(vp, st);
        drawScene(system, mapper, camera, visuals, vp, targetPx, timeSeconds, timeDirection, layers, selectedBody, st);
    }
}

void SceneRenderer::drawScene(const sim::SolarSystem& system, const ScaleMapper& mapper, const OrbitCamera& camera,
                              const std::vector<BodyVisual>& visuals, const FrameViewport& vp, glm::vec2 targetPx,
                              double timeSeconds, float timeDirection, const SceneLayers& layers,
                              int selectedBody, const style::SceneStyle& st) const {
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_PROGRAM_POINT_SIZE);

    const float aspect = vp.aspect();
    const glm::mat4 proj = camera.projection(aspect);
    const glm::mat4 view = camera.view();
    const glm::mat4 viewProj = proj * view;
    const glm::vec3 target = glm::vec3(camera.target());
    const glm::vec3 eye = camera.eyeOffset();
    const float time = static_cast<float>(std::fmod(timeSeconds, 3600.0));

    // Depth fade range, from the camera distance and the size of the system.
    const float camDist = camera.distance();
    const float fadeNear = camDist * kFadeNearFactor;
    const float fadeFar = camDist * kFadeFarFactor + 0.45f * mapper.systemExtent();

    // --- starfield: at infinity, additive, no depth (so no depth fade either) --------
    if (layers.stars) {
        drawStarfield(view, proj, eye, vp, time, st);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (layers.grid) {
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // --- gravity-well grid: secondary texture under the range rings ------------------
        const int jupiter = system.indexOfTableRow(sim::kJupiter);
        const glm::vec3 dipPos =
            jupiter >= 0 ? glm::vec3(mapper.toRender(system.body(jupiter).helioPos_AU)) : glm::vec3(1e6f);
        lineShader_.use();
        lineShader_.set("uViewProj", viewProj);
        lineShader_.set("uTarget", target);
        lineShader_.set("uEye", eye);
        lineShader_.set("uWellK", gridParams_.wellK);
        lineShader_.set("uWellSoft", gridParams_.wellSoft);
        lineShader_.set("uWellRim", gridParams_.rim());
        lineShader_.set("uYOffset", gridParams_.yOffset);
        lineShader_.set("uDipPos", dipPos);
        lineShader_.set("uDipK", gridParams_.dipK);
        lineShader_.set("uDipWidth", gridParams_.dipWidth);
        lineShader_.set("uFadeNear", fadeNear);
        lineShader_.set("uFadeFar", fadeFar);
        lineShader_.set("uFadeMin", kFadeMin);
        lineShader_.set("uColor", rgba(st.gridDim, st.gridAlpha));
        grid_.draw();

        // --- range rings, ticks, spokes, fragments (the reactor structure) ----------------
        structShader_.use();
        structShader_.set("uViewProj", viewProj);
        structShader_.set("uModel", glm::translate(glm::mat4(1.0f), glm::vec3(-target.x, structureY_ - target.y, -target.z)));
        structShader_.set("uEye", eye);
        structShader_.set("uFadeNear", fadeNear);
        structShader_.set("uFadeFar", fadeFar);
        structShader_.set("uFadeMin", kFadeMin);
        structShader_.set("uColor", rgba(st.ringDim, st.spokeAlpha));
        spokes_.draw();
        structShader_.set("uColor", rgba(st.ringDim, st.ringMinorAlpha));
        minorRings_.draw();
        structShader_.set("uColor", rgba(st.ringMid, st.ringAlpha));
        majorRings_.draw();
        structShader_.set("uColor", rgba(st.ringMid, st.fragmentAlpha));
        fragments_.draw();
        structShader_.set("uColor", rgba(st.structure, st.tickAlpha));
        ticks_.draw();

        // Dot markers near the rings.
        pointShader_.use();
        pointShader_.set("uView", view);
        pointShader_.set("uProj", proj);
        pointShader_.set("uModel", glm::translate(glm::mat4(1.0f), glm::vec3(-target.x, structureY_ - target.y, -target.z)));
        pointShader_.set("uInfinite", 0.0f);
        pointShader_.set("uPixelScale", vp.dpiScale);
        pointShader_.set("uIntensity", st.markerAlpha);
        pointShader_.set("uTime", time);
        pointShader_.set("uEye", eye);
        pointShader_.set("uFacing", 0.0f);
        pointShader_.set("uFlicker", 0.0f);
        pointShader_.set("uMode", kModeTint);
        pointShader_.set("uTint", rgb(st.structure));
        glBlendFunc(GL_ONE, GL_ONE);
        markers_.draw();
    }

    // --- bodies: opaque -------------------------------------------------------------
    int sunIndex = -1;
    for (int i = 0; i < system.bodyCount(); ++i) {
        if (system.body(i).data().kind == sim::BodyKind::Star) {
            sunIndex = i;
            break;
        }
    }
    const glm::vec3 sunRel = sunIndex >= 0 ? visuals[static_cast<std::size_t>(sunIndex)].rel : glm::vec3(0.0f);
    // A Sun that is large on screen is drawn as a particle volume (below) instead of a sphere.
    const bool sunAsParticles =
        sunIndex >= 0 && visuals[static_cast<std::size_t>(sunIndex)].radiusPx > kSunParticleModePx;

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    bodyShader_.use();
    bodyShader_.set("uViewProj", viewProj);
    bodyShader_.set("uCameraPos", eye);
    bodyShader_.set("uLightPos", sunRel);
    auto drawBody = [&](int i, const glm::vec3& colour, float emissive) {
        const BodyVisual& v = visuals[static_cast<std::size_t>(i)];
        const glm::mat4 model = glm::scale(glm::translate(glm::mat4(1.0f), v.rel), glm::vec3(v.radius));
        bodyShader_.set("uModel", model);
        bodyShader_.set("uColor", colour);
        bodyShader_.set("uEmissive", emissive);
        bodyShader_.set("uRimColor", rgb(i == selectedBody ? st.rimSelected : st.rimPlain));
        bodyShader_.set("uRimStrength", i == selectedBody ? 1.6f : 0.7f);
        sphere_.draw();
    };
    for (int i = 0; i < system.bodyCount(); ++i) {
        const sim::BodyData& data = system.body(i).data();
        if (data.kind == sim::BodyKind::Star) {
            if (!sunAsParticles) {
                drawBody(i, rgb(st.coreMid) * 1.3f, 1.0f); // small on screen: a bright disc
            }
            continue;
        }
        drawBody(i, rgb(data.colorRGB), 0.0f); // planets keep their natural colours
    }

    // --- Sun as reactor core: glow, particle volume, halo rings, crown ------------------
    if (sunIndex >= 0) {
        const BodyVisual& sv = visuals[static_cast<std::size_t>(sunIndex)];
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);

        if (sunAsParticles) {
            drawBody(sunIndex, rgb(st.coreFalloff) * 0.25f, 1.0f); // soft glowing core; particles show through
            glDisable(GL_CULL_FACE);
            const float spin = static_cast<float>(std::fmod(timeSeconds * 0.05, 6.283185307));
            glm::mat4 model = glm::translate(glm::mat4(1.0f), sv.rel);
            model = glm::rotate(model, spin, glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::scale(model, glm::vec3(sv.radius * kSunParticleScale));
            pointShader_.use();
            pointShader_.set("uView", view);
            pointShader_.set("uProj", proj);
            pointShader_.set("uModel", model);
            pointShader_.set("uInfinite", 0.0f);
            pointShader_.set("uPixelScale", vp.dpiScale);
            pointShader_.set("uIntensity", 1.25f); // HDR: dense regions feed the bloom
            pointShader_.set("uTime", time);
            pointShader_.set("uEye", eye);
            pointShader_.set("uFacing", 1.0f);
            pointShader_.set("uFlicker", 0.35f);
            pointShader_.set("uMode", kModeSun);
            pointShader_.set("uRampLow", rgb(st.coreFalloff));
            pointShader_.set("uRampMid", rgb(st.coreMid));
            pointShader_.set("uRampHigh", rgb(st.coreHot));
            sunParticles_.draw();
        }
        glDisable(GL_CULL_FACE);

        // Halo rings + tick crown, billboarded so they always frame the core.
        if (sv.radiusPx > 4.0f) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            const glm::mat4 face = billboard(view);
            const float phase = 6.2831853f * time / kHaloPulsePeriod;
            structShader_.use();
            structShader_.set("uViewProj", viewProj);
            structShader_.set("uEye", eye);
            structShader_.set("uFadeNear", 1e30f); // no depth fade on the core
            structShader_.set("uFadeFar", 2e30f);
            structShader_.set("uFadeMin", 1.0f);
            for (int i = 0; i < 3; ++i) {
                const float pulse = std::sin(phase + 2.1f * static_cast<float>(i));
                const float r = sv.radius * kHaloRadii[i] * (1.0f + kHaloPulseDepth * pulse);
                const float a = st.haloAlpha * (1.0f - 0.25f * static_cast<float>(i)) * (1.0f + kHaloPulseDepth * pulse);
                const glm::mat4 model = glm::translate(glm::mat4(1.0f), sv.rel) * face * glm::scale(glm::mat4(1.0f), glm::vec3(r));
                structShader_.set("uModel", model);
                structShader_.set("uColor", rgba(i == 0 ? st.coreMid : st.structure, a));
                haloCircle_.draw();
            }
            glm::mat4 crown = glm::translate(glm::mat4(1.0f), sv.rel) * face;
            crown = glm::rotate(crown, time * kCrownSpin, glm::vec3(0.0f, 0.0f, 1.0f));
            crown = glm::scale(crown, glm::vec3(sv.radius * kCrownRadius));
            structShader_.set("uModel", crown);
            structShader_.set("uColor", rgba(st.structure, st.haloAlpha * 0.8f));
            crown_.draw();
        }
    }

    // --- orbit rings: additive, depth-tested, no depth write ---------------------------
    if (layers.orbits) {
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        OrbitDrawParams op;
        op.viewProj = viewProj;
        op.target = target;
        op.eye = eye;
        op.viewportPx = targetPx;
        op.dpiScale = vp.dpiScale;
        op.fadeNear = fadeNear;
        op.fadeFar = fadeFar;
        op.fadeMin = kFadeMin;
        op.timeDir = timeDirection;
        op.selectedBody = selectedBody;
        op.selectColor = rgb(st.accent);
        op.plainColor = rgb(st.orbitPlain);
        op.plainAlpha = st.orbitPlainAlpha;
        orbits_.draw(orbitShader_, system, op);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_PROGRAM_POINT_SIZE);
}


void SceneRenderer::drawStarfield(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye,
                                  const FrameViewport& vp, float time, const style::SceneStyle& st) const {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    pointShader_.use();
    pointShader_.set("uView", view);
    pointShader_.set("uProj", proj);
    pointShader_.set("uModel", glm::mat4(1.0f));
    pointShader_.set("uInfinite", 1.0f);
    pointShader_.set("uPixelScale", vp.dpiScale);
    pointShader_.set("uIntensity", 0.80f);
    pointShader_.set("uTime", time);
    pointShader_.set("uEye", eye);
    pointShader_.set("uFacing", 0.0f);
    pointShader_.set("uFlicker", 0.10f); // faint twinkle
    pointShader_.set("uMode", kModeStars);
    pointShader_.set("uRampLow", rgb(st.starCool));
    pointShader_.set("uRampMid", rgb(st.starMid));
    pointShader_.set("uRampHigh", rgb(st.starWarm));
    stars_.draw();
}

void SceneRenderer::renderEarth(const EarthFrame& frame, const SceneLayers& layers, double timeSeconds,
                                const style::SceneStyle& st) {
    const FrameViewport& vp = frame.viewport;
    const glm::vec3 bg = rgb(st.bgMid);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(vp.framebufferPx.x), static_cast<GLsizei>(vp.framebufferPx.y));
    glClearColor(bg.r, bg.g, bg.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float k = vp.fbPerWindow();
    const glm::vec2 viewFb = vp.viewSizeFb();
    const int w = std::max(1, static_cast<int>(viewFb.x));
    const int h = std::max(1, static_cast<int>(viewFb.y));
    const int x = static_cast<int>(vp.viewMin.x * k);
    const int y = static_cast<int>(vp.framebufferPx.y - (vp.viewMin.y + vp.viewSize.y) * k);
    const glm::vec2 targetPx(static_cast<float>(w), static_cast<float>(h));
    const float time = static_cast<float>(std::fmod(timeSeconds, 3600.0));

    auto drawContents = [&]() {
        drawBackground(vp, st);
        if (layers.stars) {
            drawStarfield(frame.camera->view(), frame.camera->projection(vp.aspect()), frame.camera->eyeOffset(), vp,
                          time, st);
        }
        earth_.draw(frame, targetPx, timeSeconds, st);
    };

    if (post_.begin(w, h)) {
        glClear(GL_DEPTH_BUFFER_BIT);
        drawContents();
        PostSettings ps;
        ps.bloom = layers.bloom;
        ps.finish = layers.finish;
        ps.warmth = layers.warmth;
        ps.shadowLift = rgb(st.gradeShadow) * st.gradeShadowLift;
        ps.highlight = rgb(st.gradeHighlight);
        ps.redLimit = st.gradeRedLimit;
        post_.end(ps, x, y, static_cast<float>(std::fmod(timeSeconds, 1000.0)));
    } else {
        glViewport(x, y, w, h);
        drawContents();
    }
}

} // namespace render
