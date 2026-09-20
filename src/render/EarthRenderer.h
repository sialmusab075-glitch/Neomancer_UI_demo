#pragma once

#include "neo/sim/EarthFlybys.h"
#include "render/Camera.h"
#include "render/FrameViewport.h"
#include "render/LineMesh.h"
#include "render/Picking.h"
#include "render/Shader.h"
#include "render/Texture.h"
#include "style/SceneStyle.h"

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>
#include <vector>

namespace render {

// Earth view: the Earth fixed at the centre, turning on its axis, with the
// query's flybys drawn around it. The maths that decides WHERE things are lives
// in neo/sim/EarthFlybys (pure, tested); this file only draws it.

constexpr float kEarthTiltRadians = 0.40910518f; // 23.44 degrees
constexpr float kEarthViewMinDistance = 1.7f;    // just above the surface
constexpr float kEarthViewMaxDistance = 90.0f;

// The fixed Sun direction for the day/night terminator.
glm::vec3 earthSunDirection();

// Tilt then spin: the model matrix of the planet (radius 1). Spin is about the tilted axis.
glm::mat4 earthModelMatrix(float spinRadians);

// A point on a reference ring where its label sits, in render units.
glm::vec3 ringLabelPoint(const neo::ReferenceRing& ring, const neo::EarthViewScale& scale);

// --- CPU-side layout: no OpenGL, shared by drawing, picking and the HUD overlay ---

struct FlybyScreen {
    ScreenPoint sp;          // window pixels
    bool  visible = false;   // inside its drawn path at this time
    bool  occluded = false;  // hidden behind the Earth from this camera
    float alpha = 1.0f;      // fades in and out at the path ends
    glm::vec3 rel{0.0f};     // render-space position
};

void layoutFlybys(const neo::FlybyScene& scene, double jdNow, const OrbitCamera& camera, const FrameViewport& vp,
                  std::vector<FlybyScreen>& out);

// Nearest visible, unoccluded marker under a window-pixel position, or -1.
int pickFlyby(const neo::FlybyScene& scene, const std::vector<FlybyScreen>& layout, const FrameViewport& vp,
              float windowX, float windowY);

struct EarthFrame {
    const OrbitCamera*    camera = nullptr;
    FrameViewport         viewport;
    double                spinRadians = 0.0;
    double                jdNow = 0.0;
    const neo::FlybyScene* scene = nullptr;   // may be null: just the Earth
    const std::vector<FlybyScreen>* layout = nullptr;
    int                   selected = -1;
    int                   hovered = -1;
    glm::vec3             earthColour{0.2f, 0.4f, 0.8f}; // the body table's colour: grid fallback and atmosphere tint
    bool                  showRings = true;
};

class EarthRenderer {
public:
    EarthRenderer() = default;
    ~EarthRenderer();
    EarthRenderer(const EarthRenderer&) = delete;
    EarthRenderer& operator=(const EarthRenderer&) = delete;

    // Shaders and meshes; a missing texture is NOT an error (the procedural grid
    // takes over, and textureError() says why).
    bool init(std::string& error);
    void destroy();

    bool hasTexture() const { return texture_.valid(); }
    const std::string& textureError() const { return textureError_; }

    // Rebuilds the path meshes. Called only when the query result changes.
    void setScene(const neo::FlybyScene& scene);
    void clearScene();

    // Draws into the currently bound target (viewport already set by the caller).
    void draw(const EarthFrame& frame, glm::vec2 targetPx, double timeSeconds, const style::SceneStyle& style);

private:
    struct PathRange {
        int    mesh = 0; // 0 = PHA, 1 = other
        GLint  first = 0;
        GLsizei count = 0;
    };
    struct MarkerVertex {
        glm::vec3 pos;
        float     size;
        glm::vec4 colour;
        float     hollow;
    };

    void createSphere();
    void drawMarkers(const EarthFrame& frame, const glm::mat4& viewProj, const style::SceneStyle& style);

    Shader earthShader_;
    Shader atmosShader_;
    Shader ringShader_;
    Shader pathShader_;     // widened by a geometry shader: the selected and hovered paths
    Shader pathThinShader_; // native 1 px lines: every other path
    Shader markerShader_;
    Texture2D texture_;
    std::string textureError_;

    GLuint sphereVao_ = 0, sphereVbo_ = 0, sphereEbo_ = 0;
    GLsizei sphereIndexCount_ = 0;

    LineMesh circle_;
    LineMesh pathsPha_;
    LineMesh pathsOther_;
    std::vector<PathRange> ranges_; // one per flyby, parallel to FlybyScene::flybys

    GLuint markerVao_ = 0, markerVbo_ = 0;
    std::vector<MarkerVertex> markers_; // rebuilt every frame, capacity kept
    std::size_t markerCapacity_ = 0;

    neo::EarthViewScale scale_;
};

} // namespace render
