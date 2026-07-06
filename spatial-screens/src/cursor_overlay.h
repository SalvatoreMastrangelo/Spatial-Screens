// Software cursor overlay: neither capture path delivers the pointer
// (mutter on X11 ignores the portal's embedded cursor_mode; XShm never
// grabs it), so blend the XFixes cursor image over the captured BGRX
// pixels ourselves. Shared by the render loop (portal frames, staging
// buffer, save/restore for tick-rate cursor motion) and the xshm grab
// thread (fresh grab every frame, no save needed).
#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Saved patch of pixels beneath the last cursor stamp, so the pointer can
// move at the tick rate between (possibly slower) content frames: restore
// the patch, re-blend at the new position, re-upload.
struct CursorUnder {
    std::vector<uint8_t> px;
    int x = 0, y = 0, w = 0, h = 0;  // rect in frame coords
    bool valid = false;
};

inline void cursor_restore(CursorUnder& u, uint8_t* dst, uint32_t pitch) {
    if (!u.valid) return;
    for (int row = 0; row < u.h; row++)
        memcpy(dst + size_t(u.y + row) * pitch + size_t(u.x) * 4,
               u.px.data() + size_t(row) * u.w * 4, size_t(u.w) * 4);
    u.valid = false;
}

// Alpha-blend the XFixes cursor (premultiplied ARGB; rows of unsigned long,
// low 32 bits per pixel) over BGRX pixels. sx/sy = captured region origin
// in root space. When `u` is non-null the covered rect is saved into it
// first (for cursor_restore); pass null for a frame that is grabbed fresh
// every time.
inline void composite_cursor(Display* dpy, uint8_t* dst, int w, int h,
                             uint32_t pitch, int sx, int sy, CursorUnder* u) {
    XFixesCursorImage* ci = XFixesGetCursorImage(dpy);
    if (!ci) return;
    int cx = ci->x - ci->xhot - sx, cy = ci->y - ci->yhot - sy;
    int x0 = std::max(cx, 0), y0 = std::max(cy, 0);
    int x1 = std::min(cx + ci->width, w), y1 = std::min(cy + ci->height, h);
    if (x0 >= x1 || y0 >= y1) { XFree(ci); return; }
    if (u) {
        u->x = x0; u->y = y0; u->w = x1 - x0; u->h = y1 - y0;
        u->px.resize(size_t(u->w) * u->h * 4);
        for (int row = 0; row < u->h; row++)
            memcpy(u->px.data() + size_t(row) * u->w * 4,
                   dst + size_t(u->y + row) * pitch + size_t(u->x) * 4, size_t(u->w) * 4);
        u->valid = true;
    }
    for (int dy = y0; dy < y1; dy++) {
        uint32_t* out = reinterpret_cast<uint32_t*>(dst + size_t(dy) * pitch);
        const unsigned long* src = ci->pixels + size_t(dy - cy) * ci->width - cx;
        for (int dx = x0; dx < x1; dx++) {
            uint32_t s = uint32_t(src[dx]);
            uint32_t a = s >> 24;
            if (!a) continue;
            uint32_t d = out[dx];
            uint32_t rb = ((d & 0x00ff00ffu) * (255 - a) / 255) & 0x00ff00ffu;
            uint32_t g = ((d & 0x0000ff00u) * (255 - a) / 255) & 0x0000ff00u;
            out[dx] = (s & 0x00ffffffu) + rb + g;  // premul src + dst*(1-a)
        }
    }
    XFree(ci);
}
