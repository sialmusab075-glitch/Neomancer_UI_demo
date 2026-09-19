#include "render/PointCloud.h"

#include <cstddef>

namespace render {

PointCloud::~PointCloud() { destroy(); }

void PointCloud::create(const std::vector<PointVertex>& points) {
    destroy();
    count_ = static_cast<GLsizei>(points.size());

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(points.size() * sizeof(PointVertex)),
                 points.data(), GL_STATIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(sizeof(PointVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(PointVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(PointVertex, size)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(PointVertex, color)));
    glBindVertexArray(0);
}

void PointCloud::destroy() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = 0;
    count_ = 0;
}

void PointCloud::draw() const {
    if (!vao_) {
        return;
    }
    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, count_);
    glBindVertexArray(0);
}

} // namespace render
