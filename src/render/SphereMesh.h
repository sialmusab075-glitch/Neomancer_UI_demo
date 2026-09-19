#pragma once

#include <glad/gl.h>

namespace render {

// Unit UV sphere (radius 1) with per-vertex normals. Shared by every body;
// each body is one draw call with its own model matrix.
class SphereMesh {
public:
    SphereMesh() = default;
    ~SphereMesh();
    SphereMesh(const SphereMesh&) = delete;
    SphereMesh& operator=(const SphereMesh&) = delete;

    void create(int stacks = 32, int slices = 64);
    void destroy();
    void draw() const;

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLsizei indexCount_ = 0;
};

} // namespace render
