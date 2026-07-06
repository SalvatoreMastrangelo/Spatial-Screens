#pragma once

// Pure two-hand "grab" math: spread between the two pinch points resizes the
// screen (diagonal inches), their midpoint repositions it in the screen's
// right/up plane. No SDK/Vulkan/socket deps — unit-tested in gesture_manip_test.
// Its own minimal vector type keeps this unit free of main.cpp's Vec3; callers
// convert at the boundary.
struct GVec3 { float x = 0.f, y = 0.f, z = 0.f; };

struct GrabState {
    bool active = false;
    float spread0 = 0.f;          // pinch-point distance at grab start
    float mid0x = 0.f, mid0y = 0.f; // pinch midpoint at grab start (image space)
    float size0 = 0.f;            // screen diagonal (inches) at grab start
    GVec3 anchor0;                // screen anchor position at grab start
    GVec3 right0, up0;            // screen basis snapshot (for reposition)
};

struct GrabResult {
    float diag = 0.f;             // new screen diagonal (inches)
    GVec3 anchor;                 // new screen anchor position
};

// Snapshot grab-start state from the two normalized-image pinch points.
GrabState grab_begin(float pLx, float pLy, float pRx, float pRy,
                     float size0, GVec3 anchor0, GVec3 right0, GVec3 up0);

// Compute new diag + anchor for the current pinch points. `distance` is the
// current screen distance (m); `gain` maps a normalized-image fraction to a
// world fraction; diag is clamped to [diag_min, diag_max].
GrabResult grab_update(const GrabState& g, float pLx, float pLy, float pRx, float pRy,
                       float distance, float gain, float diag_min, float diag_max);
