# Direct mode — glasses-on test handoff

**Build:** `master` @ `bfed237` (direct-mode merge). Rebuild if unsure: `cd spatial-screens && make`.
**Status of automated checks:** everything measurable from the console already passed — lease acquired at 1920×1200@120, fps counter 118–120, display restored on every exit path (quit, Ctrl+C, SIGTERM, failure branches), capture survives monitor resolution changes. **What's left is what only eyes behind the lenses can judge.** That's this checklist.

## Before you start

- Stop `viture-bridge` if it's running (one SDK client at a time), and make sure no other `spatial-screens` instance is alive: `pgrep -af spatial-screens`.
- Glasses plugged in and awake. If `xrandr` shows DP-1 without a mode (glasses slept):
  `xrandr --output DP-1 --mode 1920x1200 --rate 120 --pos 322x0`
- Launch: `cd spatial-screens && ./run.sh --pitch-trim 16`
- All hotkeys are **global Ctrl+Alt combos** (no window focus in direct mode).
- Panic recovery if a run ever dies hard and DP-1 stays off the desktop:
  `xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`

## Tests

Mark each ✅ / ❌ and jot a word on anything that feels off.

### 1. Tearing — the reason this branch exists
- [ ] Pan your head slowly left↔right across the virtual screen. Look for horizontal shear lines slicing the image, especially at the screen's vertical edges. Expected: **none, at any speed**. (The GLX build tore here through every compositor trick we threw at it.)

### 2. Motion-to-photon / tracking feel
- [ ] Quick head turns: the screen should feel nailed to the room, not dragging behind. Expected: at least as tight as the last GLX build, likely tighter (we flip on the glasses' own vblank now).
- [ ] Continuous slow pan: smooth motion, no stutter or frame-skips. The console fps line should sit at ~118–120 while you do this.

### 3. World-lock at rest
- [ ] Sit still and stare at screen text for ~30 s. Expected: no wiggle, shimmer, or slow drift (the One-Euro filter is unchanged — this checks the new presentation path didn't reintroduce jitter).

### 4. 6DoF translation
- [ ] Lean left/right and toward/away from the screen. Expected: correct parallax (screen behaves like a physical monitor on a desk), border stays **blue**. An **orange** border means the VIO went orientation-only — note if it happens and when.

### 5. Hotkeys
- [ ] `Ctrl+Alt+R` — screen re-places directly ahead at eye height.
- [ ] `Ctrl+Alt+Shift+R` — same, plus VIO origin reset (screen may jump once; tracking should stay live after).
- [ ] `Ctrl+Alt+[` / `]` — closer/farther. `Ctrl+Alt+-` / `=` — smaller/larger.

### 6. Capture quality
- [ ] The virtual screen shows the laptop panel live. Move a window, type in a terminal: updates at ~30 Hz, text readable at the default 24"/0.75 m. Note any color weirdness (channel swap = red/blue swapped) or aspect distortion.

### 7. OLED black
- [ ] Everything outside the virtual screen must be **pure transparent** (pixels off). Any gray haze over the room is a regression.

### 8. Fallback comparison (documents the win)
- [ ] Quit, relaunch with `./run.sh --window --pitch-trim 16`. Repeat the test-1 head pan. Expected: visibly worse — tearing and/or judder during motion (compositor paces this path to the laptop panel's 165 Hz clock, not the glasses' 120). Confirm it's still *usable* as a fallback, then quit.

### 9. Exploratory: wear detection while leased (untested territory)
The glasses drop their DP link when they sleep. What we've never observed is a sleep **while the app holds the lease** (the app kept the panel lit in all our runs, which may prevent sleep entirely — that would be finding #1).
- [ ] Run direct mode, then take the glasses off and set them down for ~2 min, watching the console on the laptop. Possible outcomes: (a) panel stays lit, nothing happens — fine; (b) draw failures accumulate and the app exits itself through teardown ("presentation failed repeatedly — shutting down") — designed behavior; (c) anything else — capture the console tail and the `xrandr` state afterward. In all cases DP-1 must be back on the desktop when the dust settles (panic command above if not).

## Results

| # | Test | Verdict | Notes |
|---|------|---------|-------|
| 1 | Tearing | | |
| 2 | Latency/smoothness | | |
| 3 | Rest stability | | |
| 4 | 6DoF parallax | | |
| 5 | Hotkeys | | |
| 6 | Capture | | |
| 7 | OLED black | | |
| 8 | Fallback compare | | |
| 9 | Sleep-while-leased | | |

Hand the filled table (or just the ❌ rows) back to Claude Code — anything failing gets a fix pass; test 9's outcome decides whether wear-detection handling goes on the phase-2 backlog.
