#include "render/LineMesh.h"

#include <cstddef>

namespace render {

LineMesh::~LineMesh() { destroy(); }

void LineMesh::create(const std::vector<LineVertex>& vertices) {
    destroy();
    count_ = static_cast<GLsizei>(vertices.size());

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(LineVertex)),
                 vertices.data(), GL_STATIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(sizeof(LineVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(LineVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(LineVertex, alpha)));
    glBindVertexArray(0);
}

void LineMesh::destroy() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = 0;
    count_ = 0;
}

void LineMesh::draw() const {
    if (!vao_) {
        return;
    }
    glBindVertexArray(vao_);
    glDrawArrays(GL_LINES, 0, count_);
    glBindVertexArray(0);
}

} // namespace render
