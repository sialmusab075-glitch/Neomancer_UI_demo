#pragma once

#include "render/Camera.h"
#include "render/EarthRenderer.h"
#include "render/EclipticGrid.h"
#include "render/FrameViewport.h"
#include "render/LineMesh.h"
#include "render/OrbitRenderer.h"
#include "render/Picking.h"
#include "render/PointCloud.h"
#include "render/PostProcess.h"
#include "render/ReactorStructure.h"
#include "render/ScaleMapper.h"
#include "render/Shader.h"
#include "render/SphereMesh.h"
#include "style/SceneStyle.h"

#include "neo/sim/EarthFlybys.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace sim {
class SolarSystem;
}

namespace render {

// Where each body ends up this frame, shared by drawing, picking and the HUD overlay.
struct BodyVisual {
    glm::vec3   rel{0.0f};      // position relative to the camera target (render units)
    float       radius = 0.0f;   // drawn radius incl. the minimum on-screen size
    float       pickRadius = 0.0f; // radius used for picking (larger minimum)
    float       radiusPx = 0.0f;   // drawn radius in framebuffer pixels
    ScreenPoint screen;          // projected centre (window pixels)
};

struct SceneLayers {
    bool orbits = true;
    bool grid = true;
    bool stars = true;
    bool bloom = true;  // HDR bloom pass
    bool finish = true; // vignette + animated noise in the composite
    float warmth = 0.6f; // colour grade strength (0..1)
};

class SceneRenderer {
public:
    static constexpr float kMinBodyPx = 3.5f;  // planets never shrink below this radius
    static constexpr float kMinSunPx = 6.0f;
    static constexpr float kMinPickPx = 12.0f; // generous click target for small bodies

    bool init(std::string& error);
    void destroy();

    // Re-creates everything that depends on the scale mode (orbit rings, grid,
    // range rings and the rest of the reference structure).
    void rebuild(const sim::SolarSystem& system, const ScaleMapper& mapper);

    std::vector<BodyVisual> layout(const sim::SolarSystem& system, const ScaleMapper& mapper,
                                   const OrbitCamera& camera, const FrameViewport& vp) const;

    // Nearest body under a window-pixel position, or -1.
    int pick(const std::vector<BodyVisual>& visuals, const OrbitCamera& camera, const FrameViewport& vp,
             float windowX, float windowY) const;

    // Draws the 3D view into the view rectangle. `timeDirection` is the sign of
    // the sim time scale (flips the orbit trails when time runs backwards).
    // All colours come from `style` (the current HUD theme's scene tokens).
    void render(const sim::SolarSystem& system, const ScaleMapper& mapper, const OrbitCamera& camera,
                const std::vector<BodyVisual>& visuals, const FrameViewport& vp, double timeSeconds,
                float timeDirection, const SceneLayers& layers, int selectedBody, const style::SceneStyle& style);

    // Range-ring labels (world render units + text), rebuilt with the scale mode.
    const std::vector<RingLabel>& ringLabels() const { return structureLabels_; }
    // Ring labels sit this far below the orbital plane (render units).
    float structureY() const { return structureY_; }

    // Projects a world position (render units) into window pixels for this frame.
    ScreenPoint projectWorld(const OrbitCamera& camera, const FrameViewport& vp, const glm::dvec3& world) const;

    // False when the driver rejected the offscreen targets (no bloom/finish, no MSAA).
    bool postProcessingAvailable() const { return post_.available(); }

    // --- Earth view: the same background, stars and HDR pipeline, a different scene ---
    // False when the Earth shaders failed to build (the solar view is unaffected).
    bool earthViewAvailable() const { return earthReady_; }
    const std::string& earthError() const { return earthError_; }
    bool hasEarthTexture() const { return earth_.hasTexture(); }
    const std::string& earthTextureError() const { return earth_.textureError(); }
    // Rebuilds the flyby path meshes; call only when the query result changes.
    void setFlybyScene(const neo::FlybyScene& scene) { earth_.setScene(scene); }
    void clearFlybyScene() { earth_.clearScene(); }
    void renderEarth(const EarthFrame& frame, const SceneLayers& layers, double timeSeconds,
                     const style::SceneStyle& style);

private:
    void drawScene(const sim::SolarSystem& system, const ScaleMapper& mapper, const OrbitCamera& camera,
                   const std::vector<BodyVisual>& visuals, const FrameViewport& vp, glm::vec2 targetPx,
                   double timeSeconds, float timeDirection, const SceneLayers& layers, int selectedBody,
                   const style::SceneStyle& style) const;
    void drawBackground(const FrameViewport& vp, const style::SceneStyle& style) const;
    // The starfield at infinity: additive, no depth. Shared by both views.
    void drawStarfield(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, const FrameViewport& vp,
                       float time, const style::SceneStyle& style) const;

    Shader bodyShader_;
    Shader orbitShader_;
    Shader lineShader_;
    Shader pointShader_;
    SphereMesh sphere_;
    OrbitRenderer orbits_;
    LineMesh grid_;
    PointCloud stars_;
    PointCloud sunParticles_;
    GridParams gridParams_;
    PostProcess post_;

    // Reference structure (static, rebuilt with the scale mode).
    Shader structShader_;
    Shader backgroundShader_;
    GLuint emptyVao_ = 0;
    LineMesh majorRings_;
    LineMesh minorRings_;
    LineMesh ticks_;
    LineMesh spokes_;
    LineMesh fragments_;
    PointCloud markers_;
    LineMesh haloCircle_; // unit circle, billboarded around the Sun
    LineMesh crown_;      // unit tick crown, billboarded around the Sun
    std::vector<RingLabel> structureLabels_;
    float structureY_ = 0.0f;

    EarthRenderer earth_;
    bool earthReady_ = false;
    std::string earthError_;
};

} // namespace render
