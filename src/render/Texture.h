#pragma once

#include <glad/gl.h>

#include <string>

namespace render {

// A 2D RGB texture loaded from a JPEG or PNG file (decoded with stb_image).
//
// The file is read here, through the UTF-8-aware app::openFile, and handed to
// stb as a memory buffer: stb's own file loading uses the ANSI fopen and would
// fail on a path with non-ASCII characters. Mipmapped, repeating in U (the seam
// of an equirectangular map) and clamped in V (the poles).
class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    // False, with the reason in `error`, when the file is missing or not decodable.
    bool loadFile(const std::string& pathUtf8, std::string& error);
    void destroy();

    bool   valid() const { return id_ != 0; }
    GLuint id() const { return id_; }
    int    width() const { return width_; }
    int    height() const { return height_; }

    void bind(int unit) const;

private:
    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace render
