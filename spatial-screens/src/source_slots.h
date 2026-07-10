// Fixed source-slot bookkeeping for spatial-screens. Slot 0 is the shared
// monitor texture; window sources take slots 1..kSourceSlots-1; the renderer
// reserves index kLabelSource for the active-screen resolution label.
#pragma once

constexpr int kSourceSlots = 8;         // 0 monitor + 1..7 window
constexpr int kLabelSource = kSourceSlots;  // renderer label texture index

struct SourceSlots {
    bool used[kSourceSlots] = {};
    int alloc() {
        for (int i = 1; i < kSourceSlots; i++)
            if (!used[i]) { used[i] = true; return i; }
        return -1;
    }
    void release(int i) {
        if (i >= 1 && i < kSourceSlots) used[i] = false;
    }
};
