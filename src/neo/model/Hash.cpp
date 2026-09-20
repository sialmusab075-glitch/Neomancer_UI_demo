#include "neo/model/Hash.h"

#include <cstdio>

namespace neo {

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string fnv1a64Hex(const std::string& text) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(fnv1a64(text)));
    return std::string(buf);
}

} // namespace neo
