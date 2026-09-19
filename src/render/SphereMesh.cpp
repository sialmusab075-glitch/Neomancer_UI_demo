#include "render/SphereMesh.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace render {

SphereMesh::~SphereMesh() { destroy(); }

void SphereMesh::create(int stacks, int slices) {
    destroy();

    // Interleaved position (== normal on a unit sphere) and normal.
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    vertices.reserve(static_cast<std::size_t>((stacks + 1) * (slices + 1) * 6));
    indices.reserve(static_cast<std::size_t>(stacks * slices * 6));

    const float pi = 3.14159265358979f;
    for (int i = 0; i <= stacks; ++i) {
        const float phi = pi * static_cast<float>(i) / static_cast<float>(stacks); // 0..pi from +Y
        const float y = std::cos(phi);
        const float ring = std::sin(phi);
        for (int j = 0; j <= slices; ++j) {
            const float theta = 2.0f * pi * static_cast<float>(j) / static_cast<float>(slices);
            const float x = ring * std::cos(theta);
            const float z = ring * std::sin(theta);
            vertices.insert(vertices.end(), {x, y, z, x, y, z});
        }
    }
    const unsigned int stride = static_cast<unsigned int>(slices + 1);
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const unsigned int a = static_cast<unsigned int>(i) * stride + static_cast<unsigned int>(j);
            const unsigned int b = a + stride;
            // Counter-clockwise when seen from outside.
            indices.insert(indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    }
    indexCount_ = static_cast<GLsizei>(indices.size());

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)),
                 indices.data(), GL_STATIC_DRAW);
    const GLsizei strideBytes = static_cast<GLsizei>(6 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, strideBytes, reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, strideBytes,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);
}

void SphereMesh::destroy() {
    if (ebo_) glDeleteBuffers(1, &ebo_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = ebo_ = 0;
    indexCount_ = 0;
}

void SphereMesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace render
