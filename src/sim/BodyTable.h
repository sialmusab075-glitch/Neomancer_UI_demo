#pragma once

#include "sim/Body.h"

namespace sim {

// Rows of the body table (see BodyTable.cpp). Stable indices.
enum BodyId : int {
    kSun = 0,
    kMercury,
    kVenus,
    kEarth,
    kMars,
    kJupiter,
    kSaturn,
    kUranus,
    kNeptune,
    kBodyCount
};

const BodyData& bodyData(int index);
int bodyTableSize();
// Case-sensitive lookup by upper-case name; returns -1 if not found.
int findBody(const char* name);

} // namespace sim
