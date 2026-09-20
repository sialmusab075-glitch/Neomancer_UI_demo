#include "render/Texture.h"

#include "app/Paths.h"

// stb_image is a single header (external/stb, pinned in THIRD_PARTY.md). Only the
// two formats the project ships are compiled in, which also keeps its size down.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <cstdio>
#include <vector>

namespace render {

Texture2D::~Texture2D() { destroy(); }

void Texture2D::destroy() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
    width_ = height_ = 0;
}

bool Texture2D::loadFile(const std::string& pathUtf8, std::string& error) {
    destroy();

    std::FILE* file = app::openFile(pathUtf8, L"rb");
    if (file == nullptr) {
        error = "cannot open texture " + pathUtf8;
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        error = "texture file is empty: " + pathUtf8;
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (got != bytes.size()) {
        error = "could not read texture " + pathUtf8;
        return false;
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 3);
    if (pixels == nullptr) {
        error = std::string("cannot decode texture ") + pathUtf8 + ": " + stbi_failure_reason();
        return false;
    }

    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); // rows of RGB are not 4-byte aligned in general
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    width_ = w;
    height_ = h;
    return true;
}

void Texture2D::bind(int unit) const {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, id_);
}

} // namespace render
