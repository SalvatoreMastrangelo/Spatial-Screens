#include "gesture_manip.h"
#include <cmath>

GrabState grab_begin(float pLx, float pLy, float pRx, float pRy,
                     float size0, GVec3 anchor0, GVec3 right0, GVec3 up0) {
    GrabState g;
    g.active = true;
    g.spread0 = std::hypot(pLx - pRx, pLy - pRy);
    if (g.spread0 < 1e-4f) g.spread0 = 1e-4f;   // avoid div-by-zero on update
    g.mid0x = (pLx + pRx) * 0.5f;
    g.mid0y = (pLy + pRy) * 0.5f;
    g.size0 = size0;
    g.anchor0 = anchor0;
    g.right0 = right0;
    g.up0 = up0;
    return g;
}

GrabResult grab_update(const GrabState& g, float pLx, float pLy, float pRx, float pRy,
                       float distance, float gain, float diag_min, float diag_max) {
    float spread = std::hypot(pLx - pRx, pLy - pRy);
    float diag = g.size0 * (spread / g.spread0);   // ratio-from-start: drift-free
    if (diag < diag_min) diag = diag_min;
    if (diag > diag_max) diag = diag_max;

    float midx = (pLx + pRx) * 0.5f, midy = (pLy + pRy) * 0.5f;
    float dmx = midx - g.mid0x, dmy = midy - g.mid0y;
    float wx = dmx * gain * distance;   // rightward
    float wy = -dmy * gain * distance;  // image y is down -> world up

    GrabResult r;
    r.diag = diag;
    r.anchor.x = g.anchor0.x + g.right0.x * wx + g.up0.x * wy;
    r.anchor.y = g.anchor0.y + g.right0.y * wx + g.up0.y * wy;
    r.anchor.z = g.anchor0.z + g.right0.z * wx + g.up0.z * wy;
    return r;
}
