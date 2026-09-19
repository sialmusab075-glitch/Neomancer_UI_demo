#include "app/Screenshot.h"

#include "app/Paths.h"

#include <glad/gl.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace app {

namespace {

void put16(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    put16(b, v & 0xFFFFu);
    put16(b, v >> 16);
}

} // namespace

bool saveBackBufferBmp(const std::string& pathUtf8, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::uint32_t w = static_cast<std::uint32_t>(width);
    const std::uint32_t h = static_cast<std::uint32_t>(height);
    const std::uint32_t rowBytes = (w * 3u + 3u) & ~3u; // BMP rows are 4-byte aligned

    // GL rows are bottom-up, exactly like a BMP with positive height.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rowBytes) * h);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, pixels.data());

    std::vector<std::uint8_t> header;
    header.reserve(54);
    header.push_back('B');
    header.push_back('M');
    put32(header, 54u + rowBytes * h); // file size
    put32(header, 0u);                 // reserved
    put32(header, 54u);                // pixel data offset
    put32(header, 40u);                // BITMAPINFOHEADER size
    put32(header, w);
    put32(header, h);
    put16(header, 1u);                 // planes
    put16(header, 24u);                // bits per pixel
    put32(header, 0u);                 // BI_RGB
    put32(header, rowBytes * h);
    put32(header, 2835u);              // 72 DPI
    put32(header, 2835u);
    put32(header, 0u);
    put32(header, 0u);

    std::FILE* f = openFile(pathUtf8, L"wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(header.data(), 1, header.size(), f) == header.size() &&
                    std::fwrite(pixels.data(), 1, pixels.size(), f) == pixels.size();
    std::fclose(f);
    return ok;
}

} // namespace app
