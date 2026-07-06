# SBS / 3D mode switch — glasses-on spike handoff

**Goal:** answer one unverified question that gates the whole stereo-rendering
effort — *does the Luma Ultra actually enter 3840-wide side-by-side (SBS/3D)
mode when asked over the SDK on Linux, and does the OS then see a 3840-wide
DP-1?* This closes the open item in `docs/plan/phase2-spatial-screens.md:278`
("verify SBS 3840×1080 mode switching from the SDK works on Linux").

**What was built (this session):** a throwaway SDK-only spike — no rendering.
It reads the current display mode, switches the panel to 3D, holds so you can
inspect `xrandr` and the glasses, then **always restores** the original mode
(on the `--hold` timeout, on Ctrl+C, and on exit).
- `spatial-screens/src/sbs_spike.cpp`, `run-spike.sh`, Makefile target `sbs-spike`.
- Builds and links against the SDK clean.

**Build:** `cd spatial-screens && make sbs-spike`

---

## RESULT — 🟢 VERIFIED 2026-07-05 (glasses-on, `switch_dimension(true)`)

SBS/3D switching works end-to-end on Linux, better than hoped — the OS adopts
the 3840-wide mode on its own. This closes `phase2-spatial-screens.md:278` and
green-lights the ~2–4 day stereo-rendering effort.

| Signal | Result |
|---|---|
| `switch_dimension(is_3d=true)` | `rc 0` SUCCESS |
| SDK `get_display_mode` BEFORE | `0x44` = 1920×1200@120 (native 2D) |
| OS saw a 3840-wide DP-1? | ✅ `xrandr` during hold: `DP-1 3840x1080+2560+0`, `3840x1080 60.00*+` (auto-adopted, no forced modeline) |
| Per-eye routing (eyes-on) | ✅ each eye showed **one half** of the image |
| Auto-restore | ✅ `set_display_mode(0x44)` `rc 0`; DP-1 confirmed back to `1920x1200@120` |

**Two findings to carry into the design:**
1. **`get_display_mode` is laggy — do NOT poll it for synchronous confirmation.**
   It trailed reality by a full command cycle: reported 2D right after the
   switch to 3D, then reported 3D (`0x32`) *after* the restore to 2D. Trust the
   command return codes + `xrandr`, not `get_display_mode`.
2. **Two SBS modes verified; use the native one.** `switch_dimension(true)` gives
   3840×1080@60 (1920×1080/eye, letterboxed on the 1200 panel, half the 120 Hz).
   **`set_display_mode(0x45)` gives 3840×1200@90** — `rc 0`, X adopted
   `3840x1200 90.00*+`, eyes-on "looked fuller" (native 1920×1200/eye, no
   letterbox). **The design should use 0x45**: full per-eye resolution, refresh
   cost only 120→90 (not 120→60). `0x42` (3840×1200@60) untested — 0x45 is
   strictly better so it's moot unless @90 causes trouble on hardware.
3. **The mode switch reflows the desktop layout.** The wider output jumps to
   `+2560+0` in 3D, and GNOME re-stacked eDP-1/DP-1 across the runs. Cosmetic,
   but the `spatial-screens` integration should expect (and ideally pin) the
   layout around the switch.

No fused 3D was expected in this test — the panel sliced the flat 2D desktop, so
each eye saw a different half (correct). Producing real depth is the renderer's
job (two eye views into the 3840-wide framebuffer).

---

## Safety first (this box has bitten us before)

Switching DP-1 to a 3840-wide mode is a real modeset. If the glasses black out
or X gets confused you must not lose control of the machine:

- **Run the spike from the laptop panel or over SSH — never with your only
  terminal on the glasses.** If DP-1 is currently your *primary* X output
  (per `docs/specs/2026-07-03-direct-mode-design.md`), consider making the
  laptop panel primary first, so a glasses blackout can't lock you out.
- Have a recovery path ready: tailscale SSH (`ssh sonmorri`) +
  `sudo systemctl restart gdm` if the whole stack hangs. (See the NVIDIA
  idle-blank DPMS hang note in the M3 handoff — inhibit idle blanking before
  starting: `gnome-session-inhibit --inhibit idle sleep 3600 &`.)
- **Panic restore** if a run dies hard and DP-1 stays wrong:
  `xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`
- One SDK client at a time: `pkill viture-bridge; pgrep -af spatial-screens`
  (make sure nothing else holds the SDK). Glasses plugged in and awake.

---

## Run it

Open **two** terminals (both on the laptop panel / SSH).

**Terminal A — the spike:**
```
cd spatial-screens
./run-spike.sh              # default: switch_dimension(true) -> documented 3840x1080@60
```
Read its console. It prints the mode BEFORE (record this — it's the Luma's
native 2D mode constant), the `switch_dimension` return code, the mode AFTER
(sampled ~3s), then holds 20s.

**Terminal B — while the spike holds:**
```
xrandr                      # does DP-1 list / show a 3840-wide mode?
xrandr --output DP-1 --verbose | sed -n '1,40p'   # modes + current
```
And **look through the glasses**: with the panel in SBS but X still scanning
out 1920-wide, the image will likely look doubled/half-width/wrong — that
itself is evidence the panel switched. If `xrandr` offers a 3840 mode, try
`xrandr --output DP-1 --mode 3840x1080` (or 3840x1200) and check whether the
two eyes then show **independent** halves.

**Then try the native-height variants** (Luma is 1920×1200/eye, so the 1080
modes letterbox):
```
./run-spike.sh --mode 0x45   # 3840x1200@90 SBS
./run-spike.sh --mode 0x42   # 3840x1200@60 SBS
./run-spike.sh --hold 40     # more time to poke around
```

The spike restores automatically. After each run confirm DP-1 is back to its
original mode (`xrandr`); use the panic command if not.

---

## What each outcome means (this is the decision the spike exists to make)

| `switch_dimension` / `set_display_mode` rc | `xrandr` after | Read | Effect on estimate |
|---|---|---|---|
| `0 SUCCESS` | DP-1 gains / shows a **3840-wide** mode | 🟢 **Path works** | 2–4 day stereo estimate stands |
| `0 SUCCESS` | DP-1 **unchanged** (still 1920-wide, no 3840 mode) | 🟡 Panel switched but Linux/DP won't scan out 3840 | Add EDID/modeline/forced-mode work — estimate grows; dig into why the 3840 mode isn't offered |
| `-3` mode incorrect / `-2` USB N/A / other neg | — | 🔴 SDK doesn't drive the Luma this way on Linux | Rethink approach before any stereo design |

Also note **which** modes are accepted: if `switch_dimension(true)` (1080) works
but `--mode 0x45/0x42` (1200) is rejected, we're limited to 1080-height SBS
(letterboxed on the 1200 panel) — worth knowing for the design.

---

## Results — filled 2026-07-05

- Native 2D mode BEFORE (from console): `0x44`  (1920×1200@120)
- `./run-spike.sh` (switch_dimension true): rc `0`, xrandr showed 3840? **Y** —
  `DP-1 3840x1080+2560+0`, `3840x1080 60.00*+`. (SDK `get_display_mode` AFTER
  was unreliable/lagged — see finding #1 above; ignore it.)
- Glasses in SBS looked: **image changed visibly; each eye got one half** of the
  image (true per-eye SBS routing). No fusion expected — flat 2D content.
- `--mode 0x45` (3840×1200@90): rc `0`, xrandr `DP-1 3840x1200 90.00*+`,
  eyes-on **"looked fuller"** (native height). ✅ **This is the mode to use.**
- `--mode 0x42` (3840×1200@60): **not run** — 0x45 @90 is strictly better.
- DP-1 restored cleanly? **Y** (both runs) — back to `1920x1200+322+0 @120`.
- Overall verdict: **🟢** — SBS switching works end-to-end on Linux; OS
  auto-adopts the 3840-wide mode; native 1920×1200/eye @90 available.

**Design goal (decided 2026-07-05):** the target is **multiple virtual screens
racked at different depths** — real depth separation you can't fake — NOT just
making one flat screen feel solid (a single screen gains little from stereo,
since 6DoF motion parallax already sells its distance, and it'd cost 120→90 Hz
for a subtle gain). So the design must move `spatial-screens` from its current
single world-anchored quad to a **multi-screen scene** with a per-screen
distance/pose, then render that scene stereo.

**Next:** write `docs/specs/2026-07-05-stereo-3d-design.md` scoped to that goal —
multi-screen scene graph (per-screen depth); switch to `0x45` (3840×1200@90) on
startup + restore `0x44` on exit; render two eye views (viewports `[0..1920]` /
`[1920..3840]`, camera ±IPD/2, off-axis frustum) into the 3840-wide framebuffer;
IPD as a config value (no SDK API for it); handle the desktop-layout reflow
around the switch.
