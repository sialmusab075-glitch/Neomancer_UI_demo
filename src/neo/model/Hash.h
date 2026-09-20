#pragma once

#include <cstdint>
#include <string>

namespace neo {

// FNV-1a, 64-bit. Used for cache keys and for the ingest-report checksum stored
// in neo.db. It is a fingerprint, not a security primitive: it answers "is this
// the same bytes as before", nothing more.
std::uint64_t fnv1a64(const std::string& text);

// The same value as 16 lower-case hex digits.
std::string fnv1a64Hex(const std::string& text);

} // namespace neo
