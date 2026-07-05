# Laptop display at 100% scale during spatial-screens sessions

2026-07-05

> **Superseded the same day:** the user decided 100% should be permanent, not
> per-session. `org.gnome.desktop.interface scaling-factor` is now `uint32 1`
> in dconf (explicit value — GNOME auto-HiDPI no longer applies; persists
> across reboots), and the run.sh session switch below was reverted. Kept for
> the analysis of where the 200% came from. Revert to auto with:
> `gsettings reset org.gnome.desktop.interface scaling-factor`

## Problem

The laptop panel (eDP-1, 2560x1600 @ ~200 DPI) triggers GNOME's auto-HiDPI on
X11: `org.gnome.desktop.interface scaling-factor` is `uint32 0` (auto), which
resolves to a global 200% window scale. On top of that sits
`text-scaling-factor 1.55` (fonts 155% relative to the UI; Xft.dpi 298).

At 200% the desktop only offers 1280x800 of logical real estate, so the
portal/XShm capture that spatial-screens shows in the glasses carries
low-density content. At 100% the same capture carries the full 2560x1600.

## Decisions (with user)

- **Trigger:** tied to the spatial-screens session — not glasses hotplug, not
  a manual toggle.
- **Text scaling:** `text-scaling-factor 1.55` is left untouched; only the
  window scale switches to 100%.

## Design

`spatial-screens/run.sh` gains a scale-switch block:

1. Guards: skipped when `SPATIAL_SCREENS_KEEP_SCALE=1`, when `gsettings` is
   missing, or when the session is not GNOME-on-X11 — so it degrades to a
   no-op on other setups.
2. Saves the current `org.gnome.desktop.interface scaling-factor` value
   verbatim (e.g. `uint32 0`), sets it to `1`, and registers a trap that
   restores the saved value on EXIT (with INT/TERM trapped to `exit` so the
   EXIT trap also runs on Ctrl-C / kill).
3. The former `exec` becomes a foreground run so the trap survives the app —
   restore fires on normal quit, Ctrl-C, and app crash alike.

## Caveats

- Non-GTK apps (Chrome, Electron) may not rescale until restarted; GNOME
  Shell and GTK apps follow live.
- `kill -9` / OOM of run.sh itself skips the restore. Recovery:
  `gsettings reset org.gnome.desktop.interface scaling-factor`.
- The scale is global on X11, so a GNOME-managed glasses display would also
  render at 100% — irrelevant in direct-display mode, harmless otherwise.

## Verification

Manual, per the native-side convention: stub run (real gsettings, fake app)
covering normal exit and SIGINT, then a hardware session confirming eDP-1 at
100% while spatial-screens runs and 200% auto-scale back after quit.
