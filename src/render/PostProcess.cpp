#include "render/PostProcess.h"

#include "app/Log.h"
#include "app/Paths.h"

#include <algorithm>

namespace render {

namespace {

GLuint makeTexture(int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

GLuint makeTextureFbo(GLuint tex) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return fbo;
}

bool complete() { return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE; }

} // namespace

PostProcess::~PostProcess() { destroy(); }

bool PostProcess::init(std::string& error) {
    const auto path = [](const char* name) { return app::assetPath(std::string("shaders/") + name); };
    if (!bright_.loadFromFiles(path("fullscreen.vert"), path("bloom_bright.frag"), error)) return false;
    if (!blur_.loadFromFiles(path("fullscreen.vert"), path("bloom_blur.frag"), error)) return false;
    if (!composite_.loadFromFiles(path("fullscreen.vert"), path("composite.frag"), error)) return false;
    glGenVertexArrays(1, &emptyVao_); // core profile needs a VAO even with no attributes

    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    samples_ = std::clamp(static_cast<int>(maxSamples), 1, 4);
    ready_ = true;
    return true;
}

void PostProcess::release() {
    if (msFbo_) glDeleteFramebuffers(1, &msFbo_);
    if (resolveFbo_) glDeleteFramebuffers(1, &resolveFbo_);
    glDeleteFramebuffers(2, bloomFbo_);
    if (msColor_) glDeleteRenderbuffers(1, &msColor_);
    if (msDepth_) glDeleteRenderbuffers(1, &msDepth_);
    if (resolveTex_) glDeleteTextures(1, &resolveTex_);
    glDeleteTextures(2, bloomTex_);
    msFbo_ = resolveFbo_ = msColor_ = msDepth_ = resolveTex_ = 0;
    bloomFbo_[0] = bloomFbo_[1] = bloomTex_[0] = bloomTex_[1] = 0;
    width_ = height_ = 0;
}

void PostProcess::destroy() {
    release();
    if (emptyVao_) glDeleteVertexArrays(1, &emptyVao_);
    emptyVao_ = 0;
    bright_.destroy();
    blur_.destroy();
    composite_.destroy();
    ready_ = false;
}

bool PostProcess::allocate(int w, int h) {
    release();
    width_ = w;
    height_ = h;

    glGenRenderbuffers(1, &msColor_);
    glBindRenderbuffer(GL_RENDERBUFFER, msColor_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_RGBA16F, w, h);
    glGenRenderbuffers(1, &msDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepth_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &msFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColor_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msDepth_);
    bool ok = complete();

    resolveTex_ = makeTexture(w, h);
    resolveFbo_ = makeTextureFbo(resolveTex_);
    ok = ok && complete();

    const int hw = std::max(1, w / 2);
    const int hh = std::max(1, h / 2);
    for (int i = 0; i < 2; ++i) {
        bloomTex_[i] = makeTexture(hw, hh);
        bloomFbo_[i] = makeTextureFbo(bloomTex_[i]);
        ok = ok && complete();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!ok) {
        app::logError("Post-processing framebuffers unsupported (%dx MSAA RGBA16F); bloom disabled", samples_);
        release();
        failed_ = true;
    } else {
        app::logInfo("Post-processing targets %dx%d, %dx MSAA", w, h, samples_);
    }
    return ok;
}

bool PostProcess::begin(int width, int height) {
    if (!available() || width <= 0 || height <= 0) {
        return false;
    }
    if ((width != width_ || height != height_) && !allocate(width, height)) {
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
    glViewport(0, 0, width_, height_);
    return true;
}

void PostProcess::end(const PostSettings& s, int dstX, int dstY, float timeSeconds) {
    // Resolve the multisampled scene.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo_);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(emptyVao_);
    glActiveTexture(GL_TEXTURE0);

    const int hw = std::max(1, width_ / 2);
    const int hh = std::max(1, height_ / 2);
    const bool bloom = s.bloom && s.intensity > 0.0f;
    if (bloom) {
        // Bright pass + downsample.
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[0]);
        glViewport(0, 0, hw, hh);
        bright_.use();
        bright_.set("uScene", 0);
        bright_.set("uTexel", glm::vec2(1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_)));
        bright_.set("uThreshold", s.threshold);
        bright_.set("uKnee", s.knee);
        glBindTexture(GL_TEXTURE_2D, resolveTex_);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Separable blur, ping-ponging between the two half-res targets.
        blur_.use();
        blur_.set("uTex", 0);
        const glm::vec2 texel(1.0f / static_cast<float>(hw), 1.0f / static_cast<float>(hh));
        for (int i = 0; i < s.iterations; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[1]);
            blur_.set("uDir", glm::vec2(texel.x, 0.0f));
            glBindTexture(GL_TEXTURE_2D, bloomTex_[0]);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[0]);
            blur_.set("uDir", glm::vec2(0.0f, texel.y));
            glBindTexture(GL_TEXTURE_2D, bloomTex_[1]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    // Composite into the view rectangle of the default framebuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(dstX, dstY, width_, height_);
    composite_.use();
    composite_.set("uScene", 0);
    composite_.set("uBloom", 1);
    composite_.set("uBloomIntensity", bloom ? s.intensity : 0.0f);
    composite_.set("uFinish", s.finish ? 1.0f : 0.0f);
    composite_.set("uTime", timeSeconds);
    composite_.set("uResolution", glm::vec2(static_cast<float>(width_), static_cast<float>(height_)));
    composite_.set("uWarmth", s.warmth);
    composite_.set("uShadowLift", s.shadowLift);
    composite_.set("uHighlight", s.highlight);
    composite_.set("uRedLimit", s.redLimit);
    glBindTexture(GL_TEXTURE_2D, resolveTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloomTex_[0]);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}

} // namespace render
