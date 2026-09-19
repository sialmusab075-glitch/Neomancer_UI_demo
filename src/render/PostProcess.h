#pragma once

#include "render/Shader.h"

#include <glad/gl.h>
#include <glm/vec3.hpp>

#include <string>

namespace render {

struct PostSettings {
    bool  bloom = true;
    float threshold = 0.62f;
    float knee = 0.25f;
    float intensity = 1.0f;
    int   iterations = 3;   // separable blur passes (H+V each) at half resolution
    bool  finish = true;    // vignette + animated noise

    // Colour grade (theme tokens; strength from the WARMTH slider).
    float     warmth = 0.0f;
    glm::vec3 shadowLift{0.0f};
    glm::vec3 highlight{1.0f};
    float     redLimit = 1.0f;
};

// Offscreen HDR pipeline for the 3D view:
//
//   scene -> 4x MSAA RGBA16F FBO -> blit-resolve -> RGBA16F texture
//         -> bright pass + 2x downsample -> N x (H blur, V blur) at half res
//         -> composite (scene + bloom, shoulder, vignette/noise) into the
//            default framebuffer, inside the view rectangle.
//
// MSAA is kept by rendering into a multisampled FBO and resolving it with
// glBlitFramebuffer; the default framebuffer itself is single-sampled (ImGui
// antialiases its own shapes). Targets are (re)allocated only when the view
// size changes. If the driver rejects the FBOs, available() turns false and
// the renderer draws directly to the default framebuffer instead.
class PostProcess {
public:
    PostProcess() = default;
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    bool init(std::string& error);
    void destroy();
    bool available() const { return ready_ && !failed_; }

    // Binds the multisampled scene FBO sized w x h (allocating on size change)
    // and sets the viewport. Returns false (and disables itself) on failure.
    bool begin(int width, int height);

    // Resolves, blooms and composites into the default framebuffer at
    // (dstX, dstY, width, height) in framebuffer pixels (GL origin bottom-left).
    void end(const PostSettings& s, int dstX, int dstY, float timeSeconds);

private:
    bool allocate(int width, int height);
    void release();

    Shader bright_;
    Shader blur_;
    Shader composite_;
    GLuint emptyVao_ = 0;

    GLuint msFbo_ = 0, msColor_ = 0, msDepth_ = 0;
    GLuint resolveFbo_ = 0, resolveTex_ = 0;
    GLuint bloomFbo_[2] = {0, 0};
    GLuint bloomTex_[2] = {0, 0};
    int width_ = 0, height_ = 0;
    int samples_ = 0;
    bool ready_ = false;
    bool failed_ = false;
};

} // namespace render
