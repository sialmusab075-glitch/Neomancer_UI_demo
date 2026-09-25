#include "render/EarthRenderer.h"

#include "app/Paths.h"
#include "render/GlColor.h"
#include "render/ReactorStructure.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace render {

namespace {

constexpr int kSphereStacks = 64;
constexpr int kSphereSlices = 128;
// A flyby path is a straight line with a smooth fade toward its ends, so ten
// segments are indistinguishable from forty; every segment costs a geometry-shader
// invocation per frame, times a thousand flybys. Even, so closest approach is a vertex.
constexpr int kPathSegments = 10;
constexpr float kAtmosphereScale = 1.075f;

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry helpers (no OpenGL)
// ---------------------------------------------------------------------------

glm::vec3 earthSunDirection() { return glm::normalize(glm::vec3(0.80f, 0.18f, 0.50f)); }

glm::mat4 earthModelMatrix(float spinRadians) {
    glm::mat4 m = glm::rotate(glm::mat4(1.0f), kEarthTiltRadians, glm::vec3(0.0f, 0.0f, 1.0f));
    m = glm::rotate(m, spinRadians, glm::vec3(0.0f, 1.0f, 0.0f));
    return m;
}

glm::vec3 ringLabelPoint(const neo::ReferenceRing& ring, const neo::EarthViewScale& scale) {
    const float r = static_cast<float>(neo::radialFromKm(ring.km, scale));
    const float a = -0.62f; // a fixed spot on each ring, low and to the right
    glm::vec3 p(r * std::cos(a), 0.0f, r * std::sin(a));
    if (ring.equatorial) {
        p = glm::vec3(earthModelMatrix(0.0f) * glm::vec4(p, 1.0f)); // the equatorial plane is tilted
    }
    return p;
}

void layoutFlybys(const neo::FlybyScene& scene, double jdNow, const OrbitCamera& camera, const FrameViewport& vp,
                  std::vector<FlybyScreen>& out, neo::EarthDisplay display) {
    out.resize(scene.flybys.size());
    const glm::mat4 viewProj = camera.projection(vp.aspect()) * camera.view();
    const glm::vec3 eye = camera.eyeOffset();
    const glm::dvec3 target = camera.target();
    const double half = scene.scale.pathHalfLength;

    for (std::size_t i = 0; i < scene.flybys.size(); ++i) {
        const neo::Flyby& f = scene.flybys[i];
        FlybyScreen& s = out[i];
        const double along = neo::displayAlongTrack(display, f, jdNow, scene.scale);
        s.visible = std::fabs(along) <= half;
        if (!s.visible) {
            s.occluded = false;
            s.alpha = 0.0f;
            continue;
        }
        const sim::Vec3d world = f.closest + f.direction * along;
        s.rel = glm::vec3(glm::dvec3(world.x, world.y, world.z) - target);
        s.sp = projectToScreen(viewProj, s.rel, vp.viewSize.x, vp.viewSize.y);
        s.sp.px += vp.viewMin;
        // Fade in over the first tenth of the path and out over the last.
        s.alpha = 1.0f - smoothstep(0.80f, 1.0f, static_cast<float>(std::fabs(along) / half));

        // Hidden behind the planet? The Earth is the unit sphere at the origin
        // (relative to the target, that is -target).
        const glm::vec3 toPoint = s.rel - eye;
        const float dist = glm::length(toPoint);
        Ray ray;
        ray.origin = eye;
        ray.dir = dist > 0.0f ? toPoint / dist : glm::vec3(0.0f, 0.0f, -1.0f);
        const float t = raySphere(ray, glm::vec3(-target), 1.0f);
        s.occluded = t >= 0.0f && t < dist;
    }
}

int pickFlyby(const neo::FlybyScene& scene, const std::vector<FlybyScreen>& layout, const FrameViewport& vp,
              float windowX, float windowY, const neo::FlybyChecks* checks, int selected) {
    int best = -1;
    float bestScore = 1e30f;
    const float pxToWindow = vp.dpiScale / std::max(vp.fbPerWindow(), 1e-3f);
    for (std::size_t i = 0; i < scene.flybys.size() && i < layout.size(); ++i) {
        const FlybyScreen& s = layout[i];
        if ((checks != nullptr && !checks->drawn(i, selected)) || !s.visible || s.occluded || !s.sp.inFront || !s.sp.onScreen) {
            continue;
        }
        // A generous target: the marker's radius plus a margin, never below 9 px.
        const float radius = std::max(scene.flybys[i].markerPx * 1.4f, 9.0f) * pxToWindow;
        const float dx = s.sp.px.x - windowX;
        const float dy = s.sp.px.y - windowY;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= radius && d < bestScore) {
            best = static_cast<int>(i);
            bestScore = d;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// EarthRenderer
// ---------------------------------------------------------------------------

EarthRenderer::~EarthRenderer() { destroy(); }

void EarthRenderer::createSphere() {
    struct V {
        float x, y, z, u, v;
    };
    std::vector<V> vertices(static_cast<std::size_t>((kSphereStacks + 1) * (kSphereSlices + 1)));
    for (int i = 0; i <= kSphereStacks; ++i) {
        const float v = static_cast<float>(i) / kSphereStacks;
        const float lat = 1.57079632679f - 3.14159265359f * v; // +90 degrees at v = 0 (north)
        const float y = std::sin(lat);
        const float r = std::cos(lat);
        for (int j = 0; j <= kSphereSlices; ++j) {
            const float u = static_cast<float>(j) / kSphereSlices;
            const float lon = -3.14159265359f + 6.28318530718f * u;
            // East is +X when looking at the Greenwich meridian from +Z.
            vertices[static_cast<std::size_t>(i * (kSphereSlices + 1) + j)] = {r * std::sin(lon), y, r * std::cos(lon), u, v};
        }
    }
    std::vector<unsigned int> indices;
    indices.reserve(static_cast<std::size_t>(kSphereStacks * kSphereSlices * 6));
    for (int i = 0; i < kSphereStacks; ++i) {
        for (int j = 0; j < kSphereSlices; ++j) {
            const unsigned a = static_cast<unsigned>(i * (kSphereSlices + 1) + j);
            const unsigned b = a + 1;
            const unsigned c = a + static_cast<unsigned>(kSphereSlices + 1);
            const unsigned d = c + 1;
            // Seen from outside (north up, east right): a top-left, c bottom-left, d bottom-right.
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(d);
            indices.push_back(a);
            indices.push_back(d);
            indices.push_back(b);
        }
    }
    sphereIndexCount_ = static_cast<GLsizei>(indices.size());

    glGenVertexArrays(1, &sphereVao_);
    glGenBuffers(1, &sphereVbo_);
    glGenBuffers(1, &sphereEbo_);
    glBindVertexArray(sphereVao_);
    glBindBuffer(GL_ARRAY_BUFFER, sphereVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(V)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphereEbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)), indices.data(),
                 GL_STATIC_DRAW);
    glBindVertexArray(0);
}

bool EarthRenderer::init(std::string& error) {
    const auto path = [](const char* name) { return app::assetPath(std::string("shaders/") + name); };
    if (!earthShader_.loadFromFiles(path("earth.vert"), path("earth.frag"), error)) return false;
    if (!atmosShader_.loadFromFiles(path("earth.vert"), path("atmos.frag"), error)) return false;
    if (!ringShader_.loadFromFiles(path("struct.vert"), path("line.frag"), error)) return false;
    if (!pathShader_.loadFromFiles(path("flypath.vert"), path("orbit.frag"), error, path("orbit.geom"))) return false;
    if (!pathThinShader_.loadFromFiles(path("flypath.vert"), path("flypath_thin.frag"), error)) return false;
    if (!markerShader_.loadFromFiles(path("flyby.vert"), path("flyby.frag"), error)) return false;

    createSphere();
    circle_.create(generateUnitCircle(256));

    // Room for every flyby plus a selection ring and a little spare.
    markerCapacity_ = neo::kMaxFlybys + 16;
    markers_.reserve(markerCapacity_);
    glGenVertexArrays(1, &markerVao_);
    glGenBuffers(1, &markerVbo_);
    glBindVertexArray(markerVao_);
    glBindBuffer(GL_ARRAY_BUFFER, markerVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(markerCapacity_ * sizeof(MarkerVertex)), nullptr, GL_DYNAMIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(sizeof(MarkerVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(MarkerVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(MarkerVertex, size)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(MarkerVertex, colour)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(MarkerVertex, hollow)));
    glBindVertexArray(0);

    // The texture is optional: without it the Earth is a procedural lat/long grid.
    std::string texError;
    if (!texture_.loadFile(app::assetPath("textures/earth_blue_marble_2048.jpg"), texError)) {
        textureError_ = texError;
    }
    return true;
}

void EarthRenderer::destroy() {
    texture_.destroy();
    circle_.destroy();
    pathsPha_.destroy();
    pathsOther_.destroy();
    if (sphereVbo_) glDeleteBuffers(1, &sphereVbo_);
    if (sphereEbo_) glDeleteBuffers(1, &sphereEbo_);
    if (sphereVao_) glDeleteVertexArrays(1, &sphereVao_);
    sphereVbo_ = sphereEbo_ = sphereVao_ = 0;
    if (markerVbo_) glDeleteBuffers(1, &markerVbo_);
    if (markerVao_) glDeleteVertexArrays(1, &markerVao_);
    markerVbo_ = markerVao_ = 0;
    earthShader_.destroy();
    atmosShader_.destroy();
    ringShader_.destroy();
    pathShader_.destroy();
    pathThinShader_.destroy();
    markerShader_.destroy();
}

void EarthRenderer::clearScene() {
    pathsPha_.destroy();
    pathsOther_.destroy();
    ranges_.clear();
}

void EarthRenderer::setScene(const neo::FlybyScene& scene) {
    clearScene();
    scale_ = scene.scale;
    std::vector<LineVertex> pha;
    std::vector<LineVertex> other;
    ranges_.resize(scene.flybys.size());

    const double half = scene.scale.pathHalfLength;
    for (std::size_t i = 0; i < scene.flybys.size(); ++i) {
        const neo::Flyby& f = scene.flybys[i];
        std::vector<LineVertex>& dst = f.pha ? pha : other;
        PathRange& range = ranges_[i];
        range.mesh = f.pha ? 0 : 1;
        range.first = static_cast<GLint>(dst.size());
        // A polyline of straight segments from one end of the path to the other,
        // as GL_LINES pairs; brightest at closest approach, fading toward the ends.
        auto point = [&](int k) {
            const double s = -half + 2.0 * half * static_cast<double>(k) / kPathSegments;
            const sim::Vec3d p = f.closest + f.direction * s;
            const float fade = 0.22f + 0.78f * std::pow(1.0f - static_cast<float>(std::fabs(s) / half), 1.3f);
            return LineVertex{glm::vec3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)), fade};
        };
        for (int k = 0; k < kPathSegments; ++k) {
            dst.push_back(point(k));
            dst.push_back(point(k + 1));
        }
        range.count = static_cast<GLsizei>(dst.size()) - range.first;
    }
    if (!pha.empty()) pathsPha_.create(pha);
    if (!other.empty()) pathsOther_.create(other);
}

void EarthRenderer::drawMarkers(const EarthFrame& frame, const glm::mat4& viewProj, const style::SceneStyle& st) {
    markers_.clear();
    if (frame.scene == nullptr || frame.layout == nullptr) {
        return;
    }
    const std::vector<neo::Flyby>& flybys = frame.scene->flybys;
    const glm::vec3 accent = rgb(st.accent);
    const glm::vec3 neutral = rgb(st.orbitPlain) * 1.5f;
    for (std::size_t i = 0; i < flybys.size() && i < frame.layout->size(); ++i) {
        const FlybyScreen& s = (*frame.layout)[i];
        if ((frame.checks != nullptr && !frame.checks->drawn(i, frame.selected)) || !s.visible ||
            markers_.size() + 2 > markerCapacity_) {
            continue;
        }
        const bool selected = static_cast<int>(i) == frame.selected;
        const bool hovered = static_cast<int>(i) == frame.hovered;
        glm::vec3 colour = flybys[i].pha ? accent : neutral; // PHA = accent, the rest neutral
        float size = flybys[i].markerPx;
        if (selected) {
            colour = accent * 1.6f;
            size *= 1.5f;
        } else if (hovered) {
            colour *= 1.4f;
            size *= 1.25f;
        }
        markers_.push_back({s.rel, size, glm::vec4(colour, s.alpha), flybys[i].hollow ? 1.0f : 0.0f});
        if (selected) {
            markers_.push_back({s.rel, size * 2.1f, glm::vec4(accent, 0.9f * s.alpha), 1.0f}); // selection ring
        }
    }
    if (markers_.empty()) {
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, markerVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(markers_.size() * sizeof(MarkerVertex)), markers_.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    markerShader_.use();
    markerShader_.set("uViewProj", viewProj);
    markerShader_.set("uPixelScale", frame.viewport.dpiScale);
    markerShader_.set("uIntensity", 1.35f);
    glBindVertexArray(markerVao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(markers_.size()));
    glBindVertexArray(0);
}

void EarthRenderer::draw(const EarthFrame& frame, glm::vec2 targetPx, double timeSeconds,
                         const style::SceneStyle& st) {
    (void)timeSeconds;
    const OrbitCamera& camera = *frame.camera;
    const glm::mat4 view = camera.view();
    const glm::mat4 proj = camera.projection(frame.viewport.aspect());
    const glm::mat4 viewProj = proj * view;
    const glm::vec3 eye = camera.eyeOffset();
    const glm::vec3 sun = earthSunDirection();
    const float dpi = frame.viewport.dpiScale;
    const glm::vec3 atmosphere = glm::mix(frame.earthColour, glm::vec3(0.75f, 0.88f, 1.0f), 0.55f);

    glEnable(GL_MULTISAMPLE);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // --- the planet: opaque ---------------------------------------------------------------
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    const glm::mat4 planet = earthModelMatrix(static_cast<float>(frame.spinRadians));
    earthShader_.use();
    earthShader_.set("uModel", planet);
    earthShader_.set("uViewProj", viewProj);
    earthShader_.set("uSunDir", sun);
    earthShader_.set("uCameraPos", eye);
    earthShader_.set("uBaseColor", frame.earthColour);
    earthShader_.set("uGridColor", rgb(st.structure));
    earthShader_.set("uAccent", rgb(st.accent));
    earthShader_.set("uNightColor", rgb(st.bgMid));
    earthShader_.set("uAtmoColor", atmosphere);
    earthShader_.set("uHasTex", texture_.valid() ? 1.0f : 0.0f);
    if (texture_.valid()) {
        texture_.bind(0);
        earthShader_.set("uTex", 0);
    }
    glBindVertexArray(sphereVao_);
    glDrawElements(GL_TRIANGLES, sphereIndexCount_, GL_UNSIGNED_INT, nullptr);

    // --- reference rings: thin lines on the log radial scale -----------------------------------
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    if (frame.showRings) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ringShader_.use();
        ringShader_.set("uViewProj", viewProj);
        ringShader_.set("uEye", eye);
        ringShader_.set("uFadeNear", 1e30f); // no depth fade in this view
        ringShader_.set("uFadeFar", 2e30f);
        ringShader_.set("uFadeMin", 1.0f);
        const std::vector<neo::ReferenceRing>& rings = neo::referenceRings();
        for (std::size_t i = 0; i < rings.size(); ++i) {
            const float r = static_cast<float>(neo::radialFromKm(rings[i].km, scale_));
            // The circle mesh lies in the XY plane; a quarter turn about X puts it in the
            // ecliptic (XZ). GEO is then tilted with the Earth: it is the equatorial plane.
            glm::mat4 model = glm::rotate(glm::mat4(1.0f), 1.57079632679f, glm::vec3(1.0f, 0.0f, 0.0f));
            model = glm::scale(glm::mat4(1.0f), glm::vec3(r)) * model;
            if (rings[i].equatorial) {
                model = earthModelMatrix(0.0f) * model;
            }
            ringShader_.set("uModel", model);
            ringShader_.set("uColor", rings[i].equatorial ? rgba(st.structure, 0.85f)
                                                          : rgba(st.ringMid, i + 1 == rings.size() ? 0.55f : 0.80f));
            circle_.draw();
        }
    }

    // --- atmosphere shell: additive glow beyond the limb -------------------------------------------
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBlendFunc(GL_ONE, GL_ONE);
    atmosShader_.use();
    atmosShader_.set("uModel", planet * glm::scale(glm::mat4(1.0f), glm::vec3(kAtmosphereScale)));
    atmosShader_.set("uViewProj", viewProj);
    atmosShader_.set("uCameraPos", eye);
    atmosShader_.set("uSunDir", sun);
    atmosShader_.set("uAtmoColor", atmosphere);
    atmosShader_.set("uIntensity", 0.85f);
    glBindVertexArray(sphereVao_);
    glDrawElements(GL_TRIANGLES, sphereIndexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glDisable(GL_CULL_FACE);

    // --- flyby paths: additive; native 1 px lines, the selected and hovered ones widened ------------------
    if (frame.scene != nullptr) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        pathThinShader_.use();
        pathThinShader_.set("uViewProj", viewProj);
        // Additive lines pile up: the more flybys there are, the fainter each one is drawn,
        // so a thousand paths read as a field and the selected one still stands out.
        // The checked flybys are what is drawn, so the count is theirs.
        const bool subset = frame.checks != nullptr && frame.checks->size() == frame.scene->flybys.size() &&
                            !frame.checks->allChecked();
        const std::size_t shown = subset ? frame.checks->drawnCount(frame.selected) : frame.scene->flybys.size();
        const float crowd = std::clamp(std::sqrt(40.0f / static_cast<float>(std::max<std::size_t>(shown, 1))), 0.3f, 1.0f);
        // Both displays draw every checked flyby's projected path; they differ only in where the
        // markers are on those paths (real dates in PATHS, all at once in SWARM).
        if (!subset) {
            pathThinShader_.set("uColor", rgba(st.orbitPlain, 0.34f * crowd));
            pathsOther_.draw();
            pathThinShader_.set("uColor", rgba(st.accent, 0.50f * crowd));
            pathsPha_.draw();
        } else {
            // A subset: the same meshes, only the checked ranges, in one multi-draw call per mesh.
            for (int m = 0; m < 2; ++m) {
                multiFirst_[m].clear();
                multiCount_[m].clear();
            }
            for (std::size_t i = 0; i < ranges_.size(); ++i) {
                if (frame.checks->checked(i)) {
                    multiFirst_[ranges_[i].mesh].push_back(ranges_[i].first);
                    multiCount_[ranges_[i].mesh].push_back(ranges_[i].count);
                }
            }
            pathThinShader_.set("uColor", rgba(st.orbitPlain, 0.34f * crowd));
            pathsOther_.drawMulti(multiFirst_[1].data(), multiCount_[1].data(), static_cast<GLsizei>(multiFirst_[1].size()));
            pathThinShader_.set("uColor", rgba(st.accent, 0.50f * crowd));
            pathsPha_.drawMulti(multiFirst_[0].data(), multiCount_[0].data(), static_cast<GLsizei>(multiFirst_[0].size()));
        }

        // The selected and hovered paths: a soft halo and a bright core.
        pathShader_.use();
        pathShader_.set("uViewProj", viewProj);
        pathShader_.set("uViewport", targetPx);
        auto highlight = [&](int index, float strength) {
            if (index < 0 || static_cast<std::size_t>(index) >= ranges_.size()) {
                return;
            }
            const PathRange& r = ranges_[static_cast<std::size_t>(index)];
            const LineMesh& mesh = r.mesh == 0 ? pathsPha_ : pathsOther_;
            pathShader_.set("uSoft", 1.0f);
            pathShader_.set("uWidth", 5.0f * dpi);
            pathShader_.set("uColor", rgba(st.accent, 0.30f * strength));
            mesh.drawRange(r.first, r.count);
            pathShader_.set("uSoft", 0.0f);
            pathShader_.set("uWidth", 2.0f * dpi);
            pathShader_.set("uColor", glm::vec4(rgb(st.accent) * 1.5f, 0.95f * strength));
            mesh.drawRange(r.first, r.count);
        };
        if (frame.hovered != frame.selected) {
            highlight(frame.hovered, 0.6f);
        }
        highlight(frame.selected, 1.0f);
    }

    // --- markers ----------------------------------------------------------------------------------------------
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    drawMarkers(frame, viewProj, st);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_PROGRAM_POINT_SIZE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace render
