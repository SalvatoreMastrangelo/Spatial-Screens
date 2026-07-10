#include "text_raster.h"

namespace {
constexpr int CW = 5, CH = 7, GAP = 1;

// Each glyph: 7 bytes, bit 4 (0x10) = leftmost column, bit 0 = rightmost.
struct Glyph { char c; uint8_t rows[CH]; };
const Glyph FONT[] = {
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'x', {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}},  // multiplication cross
};
const Glyph* find_glyph(char c) {
    for (auto& g : FONT) if (g.c == c) return &g;
    return nullptr;
}
}  // namespace

RasterBuf text_raster_render(const std::string& s, uint32_t fg, uint32_t bg, int scale) {
    if (scale < 1) scale = 1;
    // Count renderable glyphs for the width.
    int n = 0;
    for (char c : s) if (find_glyph(c)) n++;
    RasterBuf out;
    if (n == 0) return out;
    out.w = (n * CW + (n - 1) * GAP) * scale;
    out.h = CH * scale;
    out.data.assign(size_t(out.w) * out.h * 4, 0);
    auto put = [&](int x, int y, uint32_t px) {
        uint8_t* p = &out.data[(size_t(y) * out.w + x) * 4];
        p[0] = px & 0xFF; p[1] = (px >> 8) & 0xFF; p[2] = (px >> 16) & 0xFF; p[3] = 0xFF;
    };
    // Background fill.
    for (int y = 0; y < out.h; y++) for (int x = 0; x < out.w; x++) put(x, y, bg);
    int penx = 0;
    for (char c : s) {
        const Glyph* g = find_glyph(c);
        if (!g) continue;
        for (int gy = 0; gy < CH; gy++)
            for (int gx = 0; gx < CW; gx++)
                if (g->rows[gy] & (1 << (CW - 1 - gx)))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            put((penx + gx) * scale + sx, gy * scale + sy, fg);
        penx += CW + GAP;
    }
    return out;
}
