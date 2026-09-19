#pragma once

#include "render/Starfield.h"

#include <glad/gl.h>

#include <vector>

namespace render {

// GPU buffer of point sprites (PointVertex), drawn with points.vert/frag.
class PointCloud {
public:
    PointCloud() = default;
    ~PointCloud();
    PointCloud(const PointCloud&) = delete;
    PointCloud& operator=(const PointCloud&) = delete;

    void create(const std::vector<PointVertex>& points);
    void destroy();
    void draw() const;

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLsizei count_ = 0;
};

} // namespace render
