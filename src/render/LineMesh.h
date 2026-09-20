#pragma once

#include "render/EclipticGrid.h"

#include <glad/gl.h>

#include <vector>

namespace render {

// GPU buffer of a GL_LINES list with per-vertex opacity (LineVertex),
// drawn with line.vert/frag. Used for the ecliptic grid.
class LineMesh {
public:
    LineMesh() = default;
    ~LineMesh();
    LineMesh(const LineMesh&) = delete;
    LineMesh& operator=(const LineMesh&) = delete;

    void create(const std::vector<LineVertex>& vertices);
    void destroy();
    void draw() const;
    // Draws only vertices [first, first + count): one path out of many sharing a buffer.
    void drawRange(GLint first, GLsizei count) const;
    // Several ranges in ONE call (glMultiDrawArrays): the checked flybys of the Earth view.
    void drawMulti(const GLint* firsts, const GLsizei* counts, GLsizei drawCount) const;

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLsizei count_ = 0;
};

} // namespace render
