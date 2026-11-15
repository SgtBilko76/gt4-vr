#pragma once

#include "gs.hpp"

namespace ps2::gs::raster {
extern std::uint64_t g_pixels_written;

void reset();
void draw(const State &s, const Vertex *v, int count);

}
