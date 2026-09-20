// Render-side geometry tests: scale mapping, picking, projection and the
// procedural generators. Pure math (GLM), no window or GPU needed.

#include "render/Camera.h"
#include "render/EclipticGrid.h"
#include "render/Picking.h"
#include "render/ReactorStructure.h"
#include "render/ScaleMapper.h"
#include "render/SwarmPick.h"
#include "render/Starfield.h"
#include "sim/BodyTable.h"
#include "sim/Constants.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

std::string fmt(const char* f, double a, double b = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

void testScaleMapper() {
    std::printf("[scale] compressed and true-scale mapping\n");
    render::ScaleMapper m;

    // Compressed: d = k * log10(1 + c * d_AU), radial.
    const glm::dvec3 e = m.toRender(sim::Vec3d(1.0, 0.0, 0.0));
    check(std::fabs(glm::length(e) - 10.0 * std::log10(11.0)) < 1e-12, "compressed 1 AU distance",
          fmt("%.9f", glm::length(e)));
    const glm::dvec3 n = m.toRender(sim::Vec3d(30.0, 0.0, 0.0));
    check(std::fabs(glm::length(n) - 10.0 * std::log10(301.0)) < 1e-12, "compressed 30 AU distance");
    // Direction preserved, axes: ecliptic (x, y, z) -> render (x, z, -y).
    const glm::dvec3 d = m.toRender(sim::Vec3d(0.0, 2.0, 0.0));
    check(std::fabs(d.x) < 1e-12 && std::fabs(d.y) < 1e-12 && d.z < 0.0, "ecliptic +y maps to render -z");
    const glm::dvec3 up = m.toRender(sim::Vec3d(0.0, 0.0, 1.0));
    check(up.y > 0.0, "ecliptic north maps to render +y");
    check(m.toRender(sim::Vec3d(0.0, 0.0, 0.0)) == glm::dvec3(0.0), "origin stays at origin");

    // Orbit direction is preserved: prograde (counter-clockwise seen from ecliptic
    // north) stays counter-clockwise seen from render +y.
    const glm::dvec3 a = m.toRender(sim::Vec3d(1.0, 0.0, 0.0));
    const glm::dvec3 b = m.toRender(sim::Vec3d(0.0, 1.0, 0.0));
    check(glm::cross(a, b).y > 0.0, "handedness preserved (prograde stays prograde)");

    // Compressed radii: exaggerated, monotonic, Sun capped.
    const float rEarth = m.bodyRadius(sim::bodyData(sim::kEarth));
    const float rJupiter = m.bodyRadius(sim::bodyData(sim::kJupiter));
    const float rMercury = m.bodyRadius(sim::bodyData(sim::kMercury));
    const float rSun = m.bodyRadius(sim::bodyData(sim::kSun));
    check(std::fabs(rEarth - 0.25f) < 1e-6f, "Earth compressed radius 0.25");
    check(rMercury < rEarth && rEarth < rJupiter, "compressed radii are monotonic");
    check(rSun <= m.sunRadiusCap + 1e-6f, "Sun radius capped");

    // True scale: real ratios.
    m.mode = render::ScaleMode::True;
    check(std::fabs(glm::length(m.toRender(sim::Vec3d(5.2, 0.0, 0.0))) - 52.0) < 1e-9, "true 5.2 AU -> 52 units");
    const double ratio = static_cast<double>(m.bodyRadius(sim::bodyData(sim::kSun))) /
                         static_cast<double>(m.bodyRadius(sim::bodyData(sim::kEarth)));
    check(std::fabs(ratio - 695700.0 / 6371.0) < 1e-3, "true Sun/Earth radius ratio", fmt("%.4f", ratio));
    const double sunAU = static_cast<double>(m.bodyRadius(sim::bodyData(sim::kSun))) / m.trueUnitsPerAU;
    check(std::fabs(sunAU - 695700.0 / sim::kAU_km) < 1e-9, "true Sun radius in AU");
}

void testPicking() {
    std::printf("[pick] ray-sphere, pick ray, projection\n");
    const render::Ray r{glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    check(std::fabs(render::raySphere(r, glm::vec3(0.0f), 1.0f) - 9.0f) < 1e-5f, "head-on hit at t = 9");
    check(render::raySphere(r, glm::vec3(0.0f, 1.5f, 0.0f), 1.0f) < 0.0f, "miss beside the sphere");
    check(render::raySphere(r, glm::vec3(0.0f, 0.0f, 20.0f), 1.0f) < 0.0f, "sphere behind the ray is a miss");
    check(render::raySphere(r, glm::vec3(0.0f, 0.0f, 10.5f), 1.0f) == 0.0f, "origin inside -> t = 0");

    // A ray through the pixel where a point projects must pass through that point.
    render::OrbitCamera cam;
    cam.setDistance(40.0f);
    cam.setAngles(0.7f, 0.3f);
    const float W = 1600.0f, H = 900.0f;
    const glm::mat4 vp = cam.projection(W / H) * cam.view();
    const glm::vec3 targets[] = {glm::vec3(0.0f), glm::vec3(8.0f, 0.0f, -3.0f), glm::vec3(-12.0f, 1.0f, 6.0f)};
    for (const glm::vec3& p : targets) {
        const render::ScreenPoint sp = render::projectToScreen(vp, p, W, H);
        check(sp.inFront && sp.onScreen, "point projects on screen");
        const render::Ray ray = render::makePickRay(vp, sp.px.x, sp.px.y, W, H);
        const float t = render::raySphere(ray, p, 0.05f);
        check(t > 0.0f, "pick ray through projected pixel hits the point", fmt("t=%.4f", t));
    }
    // Screen centre looks at the target.
    const render::ScreenPoint c = render::projectToScreen(vp, glm::vec3(0.0f), W, H);
    check(std::fabs(c.px.x - W * 0.5f) < 0.01f && std::fabs(c.px.y - H * 0.5f) < 0.01f, "target at screen centre");
    // Behind the camera is not in front.
    const glm::vec3 behind = cam.eyeOffset() * 2.0f;
    check(!render::projectToScreen(vp, behind, W, H).inFront, "point behind the eye is not in front");
}

void testSwarmPick() {
    std::printf("[swarm pick] the NEOS layer's CPU picker\n");
    render::OrbitCamera cam;
    cam.setDistance(30.0f);
    cam.setAngles(0.7f, 0.3f);
    render::ScaleMapper mapper; // compressed, like the default view
    render::FrameViewport vp;
    vp.framebufferPx = glm::vec2(1600.0f, 900.0f);
    vp.windowPx = glm::vec2(1600.0f, 900.0f);
    vp.viewMin = glm::vec2(200.0f, 40.0f); // the 3D view is not the whole window
    vp.viewSize = glm::vec2(1200.0f, 800.0f);

    // Heliocentric ecliptic AU: a few points spread around the inner system.
    const float pts[] = {1.0f, 0.0f, 0.0f,   0.0f, 1.5f, 0.2f,   -2.0f, -0.5f, 0.1f,   0.3f, -0.9f, 0.0f,   -4.0f, 3.0f, 0.5f};
    const std::size_t n = 5;
    const glm::mat4 viewProj = cam.projection(vp.aspect()) * cam.view();
    auto screenOf = [&](std::size_t i) {
        const glm::dvec3 w = mapper.toRender(sim::Vec3d{pts[3 * i], pts[3 * i + 1], pts[3 * i + 2]});
        render::ScreenPoint sp = render::projectToScreen(viewProj, glm::vec3(w - cam.target()), vp.viewSize.x, vp.viewSize.y);
        sp.px += vp.viewMin;
        return sp;
    };
    bool allFound = true;
    for (std::size_t i = 0; i < n; ++i) {
        const render::ScreenPoint sp = screenOf(i);
        allFound = allFound && sp.onScreen && render::pickSwarm(pts, n, mapper, cam, vp, sp.px.x, sp.px.y, 10.0f) == static_cast<int>(i);
    }
    check(allFound, "clicking exactly on each projected point picks that point");
    const render::ScreenPoint s2 = screenOf(2);
    check(render::pickSwarm(pts, n, mapper, cam, vp, s2.px.x + 6.0f, s2.px.y - 4.0f, 10.0f) == 2, "a click a few pixels away still picks it");
    check(render::pickSwarm(pts, n, mapper, cam, vp, s2.px.x + 40.0f, s2.px.y + 40.0f, 10.0f) == -1, "a click outside the radius picks nothing");
    check(render::pickSwarm(pts, 0, mapper, cam, vp, 500.0f, 400.0f, 10.0f) == -1, "no points: nothing");
    check(render::pickSwarm(nullptr, 3, mapper, cam, vp, 500.0f, 400.0f, 10.0f) == -1, "no buffer: nothing");

    // Two points near each other: the closer to the cursor wins.
    const float pair[] = {1.00f, 0.00f, 0.0f,   1.02f, 0.00f, 0.0f};
    render::ScreenPoint p0, p1;
    {
        const glm::dvec3 w0 = mapper.toRender(sim::Vec3d{pair[0], pair[1], pair[2]});
        const glm::dvec3 w1 = mapper.toRender(sim::Vec3d{pair[3], pair[4], pair[5]});
        p0 = render::projectToScreen(viewProj, glm::vec3(w0 - cam.target()), vp.viewSize.x, vp.viewSize.y);
        p1 = render::projectToScreen(viewProj, glm::vec3(w1 - cam.target()), vp.viewSize.x, vp.viewSize.y);
        p0.px += vp.viewMin;
        p1.px += vp.viewMin;
    }
    const float gap = std::sqrt((p0.px.x - p1.px.x) * (p0.px.x - p1.px.x) + (p0.px.y - p1.px.y) * (p0.px.y - p1.px.y));
    if (gap > 4.0f) {
        check(render::pickSwarm(pair, 2, mapper, cam, vp, p1.px.x, p1.px.y, 30.0f) == 1, "of two nearby points the one under the cursor wins", fmt("gap %.1f px", gap));
    } else {
        check(true, "(the two nearby points overlap at this zoom: nothing to distinguish)");
    }

    // A point behind the camera is never picked, however large the radius. True scale is linear, so a point
    // twice the eye offset out on the camera's side of the target is unambiguously behind the eye.
    render::ScaleMapper linear;
    linear.mode = render::ScaleMode::True;
    const glm::vec3 eye2 = cam.eyeOffset() * 2.0f; // render axes; render (x, y, z) = ecliptic (x, z, -y) * units per AU
    const float behind[] = {eye2.x / 10.0f, -eye2.z / 10.0f, eye2.y / 10.0f};
    const glm::vec3 behindRel = glm::vec3(linear.toRender(sim::Vec3d{behind[0], behind[1], behind[2]}) - cam.target());
    check(!render::projectToScreen(viewProj, behindRel, vp.viewSize.x, vp.viewSize.y).inFront, "the test point really is behind the eye");
    check(render::pickSwarm(behind, 1, linear, cam, vp, vp.viewMin.x + vp.viewSize.x * 0.5f, vp.viewMin.y + vp.viewSize.y * 0.5f, 5000.0f) == -1,
          "a point behind the eye is skipped");
}

void testGenerators() {
    std::printf("[gen] starfield, sphere shell, ecliptic grid\n");
    const auto stars = render::generateStarfield(5000, 7u);
    check(stars.size() == 5000u, "5000 stars");
    bool unit = true, sizes = true, colors = true;
    for (const auto& s : stars) {
        unit = unit && std::fabs(glm::length(s.pos) - 1.0f) < 1e-4f;
        sizes = sizes && s.size >= 1.0f && s.size <= 3.5f;
        colors = colors && s.color.a > 0.0f && s.color.a <= 1.0f;
    }
    check(unit, "stars are unit directions");
    check(sizes, "star sizes in range");
    check(colors, "star brightness in (0, 1]");
    const auto again = render::generateStarfield(5000, 7u);
    check(again[1234].pos == stars[1234].pos, "starfield is deterministic for a seed");

    const auto shell = render::generateSphereShell(2400, 1u);
    glm::vec3 centroid(0.0f);
    for (const auto& p : shell) {
        centroid += p.pos;
    }
    centroid /= static_cast<float>(shell.size());
    check(glm::length(centroid) < 0.01f, "Fibonacci shell is balanced", fmt("|c|=%.5f", glm::length(centroid)));

    render::GridParams gp;
    const auto grid = render::generateEclipticGrid(gp);
    check(grid.size() % 2 == 0, "grid is a line list");
    const std::size_t expected = static_cast<std::size_t>(2 * (gp.ringCount * gp.ringSegments +
                                                               gp.spokes * gp.spokeSegments));
    check(grid.size() == expected, "grid vertex count");
    check(render::gridHeight(gp, 0.0f) < render::gridHeight(gp, 5.0f) &&
              render::gridHeight(gp, 5.0f) < render::gridHeight(gp, 20.0f),
          "gravity well deepest at the centre");
    check(std::fabs(render::gridHeight(gp, gp.rim()) - gp.yOffset) < 1e-6f, "well meets yOffset at the rim");
    bool below = true;
    for (float r = 0.0f; r <= gp.rim(); r += 0.1f) {
        below = below && render::gridHeight(gp, r) < 0.0f;
    }
    check(below, "grid stays below the orbital plane out to the rim");
    bool flat = true;
    for (const auto& v : grid) {
        flat = flat && v.pos.y == 0.0f && v.alpha >= 0.0f && v.alpha <= 1.0f;
    }
    check(flat, "CPU grid is flat with alpha in [0,1] (the shader bends it)");

    const auto sun = render::generateSunParticles(4200, 3u);
    double meanR = 0.0;
    bool inside = true;
    for (const auto& p : sun) {
        const float r = glm::length(p.pos);
        meanR += r;
        inside = inside && r <= 1.03f;
    }
    meanR /= static_cast<double>(sun.size());
    check(sun.size() == 4200u, "4200 Sun particles");
    check(inside, "Sun particles inside the unit sphere (+jitter)");
    check(meanR < 0.8, "Sun particle density biased inward", fmt("mean r=%.3f", meanR));

    // Colour ramps are indexed by a temperature / heat value stored in rgb.r.
    bool starTemp = true, sunHeat = true;
    for (const auto& p : stars) {
        starTemp = starTemp && p.color.r >= 0.0f && p.color.r <= 1.0f;
    }
    for (const auto& p : sun) {
        sunHeat = sunHeat && p.color.r >= 0.0f && p.color.r <= 1.0f;
    }
    check(starTemp, "star temperatures in [0,1]");
    check(sunHeat, "Sun particle heat in [0,1]");
}

void testStructure() {
    std::printf("[struct] reactor range rings, ticks, spokes, fragments\n");
    render::StructureParams p;
    p.ringRadii = {1.5f, 2.2f, 3.0f, 4.1f, 8.0f, 11.0f, 15.0f, 19.0f};
    p.ringAU = {0.387, 0.723, 1.0, 1.524, 5.203, 9.537, 19.19, 30.07};
    p.innerRadius = 0.4f;
    p.outerRadius = 19.0f * 1.12f;
    const render::StructureGeometry g = render::generateStructure(p);
    const std::size_t rings = p.ringRadii.size();
    const std::size_t seg = static_cast<std::size_t>(p.ringSegments);

    check(g.majorRings.size() == rings * seg * 2, "one closed ring per planet");
    check(g.minorRings.size() == (rings + 1) * seg * 2, "intermediate rings (inner, between, rim)");
    check(g.ticks.size() == rings * 36 * 2, "36 ticks per ring (every 10 deg)");
    check(g.spokes.size() == static_cast<std::size_t>(render::kSpokeCount) * 36 * 2, "12 dashed spokes");
    check(!g.fragments.empty() && g.fragments.size() % 2 == 0, "fragments are a non-empty line list");
    check(g.markers.size() == static_cast<std::size_t>(render::kMarkerCount), "dot markers");
    check(g.labels.size() == rings, "one caption per ring");
    check(std::strcmp(g.labels[2].text, "1.00 AU") == 0, "Earth ring caption", g.labels[2].text);
    check(std::strcmp(g.labels[7].text, "30.1 AU") == 0, "outer caption uses one decimal", g.labels[7].text);

    // Ticks point outward: the second vertex of each pair is further out, and
    // every third one (30 deg) is twice as long.
    bool outward = true;
    for (std::size_t i = 0; i + 1 < g.ticks.size(); i += 2) {
        outward = outward && glm::length(g.ticks[i + 1].pos) > glm::length(g.ticks[i].pos);
    }
    check(outward, "ticks point outward");
    const float shortLen = glm::length(g.ticks[3].pos) - glm::length(g.ticks[2].pos);
    const float longLen = glm::length(g.ticks[1].pos) - glm::length(g.ticks[0].pos);
    check(std::fabs(longLen - 2.0f * shortLen) < 1e-4f, "30-deg ticks are twice as long", fmt("%.4f vs %.4f", longLen, shortLen));

    // Everything lies on the plane, inside the outer ring (+ tick length), with alpha in [0,1].
    auto sane = [&](const std::vector<render::LineVertex>& v, float rMax) {
        bool ok = true;
        for (const auto& x : v) {
            ok = ok && x.pos.y == 0.0f && glm::length(x.pos) <= rMax && x.alpha >= 0.0f && x.alpha <= 1.0f;
        }
        return ok;
    };
    const float rMax = p.outerRadius * 1.06f;
    check(sane(g.majorRings, rMax) && sane(g.minorRings, rMax) && sane(g.ticks, rMax) && sane(g.spokes, rMax) &&
              sane(g.fragments, rMax),
          "structure is planar, bounded, alpha in [0,1]");
    check(g.minorRings.back().alpha > 0.0f, "outer rim ring stays visible");
    check(g.majorRings.front().alpha > g.majorRings.back().alpha, "rings fade toward the outer edge");

    // Captions sit at different angles so close inner rings do not stack.
    bool distinct = true;
    for (std::size_t i = 1; i < g.labels.size(); ++i) {
        const glm::vec3 a = glm::normalize(g.labels[i - 1].pos);
        const glm::vec3 b = glm::normalize(g.labels[i].pos);
        distinct = distinct && glm::dot(a, b) < 0.99f;
    }
    check(distinct, "neighbouring captions are staggered in angle");

    const render::StructureGeometry again = render::generateStructure(p);
    check(again.fragments.size() == g.fragments.size() && again.fragments[5].pos == g.fragments[5].pos &&
              again.markers[17].pos == g.markers[17].pos,
          "structure is deterministic for a seed");
    p.seed = 99u;
    const render::StructureGeometry other = render::generateStructure(p);
    check(other.markers[17].pos != g.markers[17].pos, "a different seed scatters differently");

    const auto circle = render::generateUnitCircle(96);
    bool unitCircle = true;
    for (const auto& v : circle) {
        unitCircle = unitCircle && std::fabs(glm::length(v.pos) - 1.0f) < 1e-5f && v.pos.z == 0.0f;
    }
    check(circle.size() == 192u && unitCircle, "unit circle in the XY plane");
    const auto crown = render::generateCrown(72);
    check(crown.size() == 144u, "crown tick count");
    check(std::fabs(glm::length(crown[1].pos) - 1.14f) < 1e-5f && std::fabs(glm::length(crown[3].pos) - 1.07f) < 1e-5f,
          "crown: long tick every 30 deg, short between");
}

} // namespace

int main() {
    testScaleMapper();
    testPicking();
    testSwarmPick();
    testGenerators();
    testStructure();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
