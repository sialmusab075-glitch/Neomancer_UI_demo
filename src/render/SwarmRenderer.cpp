#include "render/SwarmRenderer.h"

#include "app/Paths.h"

#include <algorithm>
#include <cmath>

namespace render {

SwarmRenderer::~SwarmRenderer() { destroy(); }

bool SwarmRenderer::init(std::string& error) {
    const auto path = [](const char* name) { return app::assetPath(std::string("shaders/") + name); };
    if (!shader_.loadFromFiles(path("swarm.vert"), path("swarm.frag"), error)) {
        return false;
    }
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &posVbo_);
    glGenBuffers(1, &attrVbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, posVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, attrVbo_);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    return true;
}

void SwarmRenderer::destroy() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (posVbo_ != 0) {
        glDeleteBuffers(1, &posVbo_);
        posVbo_ = 0;
    }
    if (attrVbo_ != 0) {
        glDeleteBuffers(1, &attrVbo_);
        attrVbo_ = 0;
    }
    shader_.destroy();
    count_ = 0;
    attrs_.clear();
}

void SwarmRenderer::setObjects(const std::vector<neo::SwarmAttr>& attrs) {
    count_ = attrs.size();
    attrs_.resize(count_ * 3);
    for (std::size_t i = 0; i < count_; ++i) {
        attrs_[3 * i] = attrs[i].logDiameterKm;
        attrs_[3 * i + 1] = attrs[i].pha;
        attrs_[3 * i + 2] = attrs[i].daysToApproach;
    }
    glBindBuffer(GL_ARRAY_BUFFER, posVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count_ * 3 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, attrVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count_ * 3 * sizeof(float)), attrs_.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void SwarmRenderer::updateApproachDays(const std::vector<float>& days) {
    if (days.size() != count_) {
        return;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        attrs_[3 * i + 2] = days[i];
    }
    glBindBuffer(GL_ARRAY_BUFFER, attrVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(count_ * 3 * sizeof(float)), attrs_.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void SwarmRenderer::updatePositions(const float* xyz, std::size_t count) {
    if (count != count_ || count == 0) {
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, posVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(count * 3 * sizeof(float)), xyz);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void SwarmRenderer::draw(const SwarmDrawParams& p, const ScaleMapper& mapper, const glm::mat4& viewProj,
                         const glm::vec3& cameraTarget, float pixelScale) const {
    if (!p.enabled || count_ == 0 || vao_ == 0) {
        return;
    }
    shader_.use();
    shader_.set("uViewProj", viewProj);
    shader_.set("uTarget", cameraTarget);
    shader_.set("uTrueScale", mapper.mode == ScaleMode::True ? 1.0f : 0.0f);
    shader_.set("uK", static_cast<float>(mapper.k));
    shader_.set("uC", static_cast<float>(mapper.c));
    shader_.set("uUnitsPerAU", static_cast<float>(mapper.trueUnitsPerAU));
    shader_.set("uEarth", p.earthAu);
    shader_.set("uLegend", static_cast<int>(p.legend));
    shader_.set("uColNear", p.colNear);
    shader_.set("uColMid", p.colMid);
    shader_.set("uColFar", p.colFar);
    shader_.set("uPixelScale", pixelScale);
    shader_.set("uIntensity", p.intensity);
    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(count_));
    glBindVertexArray(0);
}

} // namespace render
