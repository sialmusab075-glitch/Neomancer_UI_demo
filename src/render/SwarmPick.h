#pragma once

#include "render/Camera.h"
#include "render/FrameViewport.h"
#include "render/ScaleMapper.h"

#include <cstddef>

namespace render {

// CPU picking for the NEOS layer (pure geometry, no OpenGL): the object whose projected
// point is nearest the cursor within `radiusPx` window pixels (the one closer to the
// camera when two are within a pixel of each other), or -1. `xyz` is 3 floats per object,
// heliocentric ecliptic AU: exactly what the layer was drawn from. Points are mapped
// with the same ScaleMapper the planets use. Called on a click, not per frame.
int pickSwarm(const float* xyz, std::size_t count, const ScaleMapper& mapper, const OrbitCamera& camera,
              const FrameViewport& vp, float windowX, float windowY, float radiusPx);

} // namespace render
