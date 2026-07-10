#include "text_raster.h"
#include <cstdio>
static int failures = 0;
#define CHECK(c) do { if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); failures++; } } while(0)

int main() {
    // Font cell 5x7, +1px inter-glyph spacing. "12" at scale 1:
    //   width = 2*5 + 1(spacing) = 11 ; height = 7.
    RasterBuf b = text_raster_render("12", 0x00FFFFFF, 0x00000000, 1);
    CHECK(b.w == 11);
    CHECK(b.h == 7);
    CHECK((int)b.data.size() == b.w * b.h * 4);

    // scale 2 doubles cell + spacing: width = 2*(5*2) + 1*2 = 22, height = 14.
    RasterBuf b2 = text_raster_render("12", 0x00FFFFFF, 0x00000000, 2);
    CHECK(b2.w == 22);
    CHECK(b2.h == 14);

    // 'x' is a valid glyph; unknown chars skipped (no width contribution).
    RasterBuf bx = text_raster_render("1x2", 0x00FFFFFF, 0, 1);
    CHECK(bx.w == 3*5 + 2);          // 3 glyphs + 2 spaces
    RasterBuf bu = text_raster_render("1?2", 0x00FFFFFF, 0, 1);
    CHECK(bu.w == 2*5 + 1);          // '?' skipped -> 2 glyphs + 1 space

    // Background fill honored on an all-unknown string of zero glyphs -> empty.
    RasterBuf be = text_raster_render("", 0, 0, 1);
    CHECK(be.w == 0 && be.h == 0 && be.data.empty());

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ok\n"); return 0;
}
