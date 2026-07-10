// Minimal on-glasses text: a hardcoded 5x7 bitmap font for '0'-'9' and 'x'
// (drawn as a multiplication sign). Renders a tight 32bpp BGRX buffer. Used
// for the active-screen resolution label (see floating-window-screens design).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct RasterBuf { std::vector<uint8_t> data; int w = 0, h = 0; };

RasterBuf text_raster_render(const std::string& s, uint32_t fg_bgrx,
                             uint32_t bg_bgrx, int scale);
