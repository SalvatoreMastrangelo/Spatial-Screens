# feat/floating-window-screens — branch resume

Worktree: `.claude/worktrees/floating-window-screens` (branched from `main` HEAD
`f38058d`, 2026-07-09). Largest of the remaining backlog features.

## Goal

A screen's source becomes `{ monitor-region | window }`. `Ctrl+Alt+W` grabs the
focused window (XComposite-redirected) onto the active screen, or spawns a new
floating screen in front when none is selected. Per-window texture; monitor
screens unchanged.

Design: [`docs/specs/2026-07-06-floating-window-screens-design.md`](../specs/2026-07-06-floating-window-screens-design.md)
(freshness-reconciled 2026-07-09 to the real `CaptureBackend` / single-texture
renderer; scope decision: **runtime spawn kept in v1, append-only**).

## State

- [x] Worktree created off `main` (not stale `origin/main`, which is 34 behind).
- [x] Precondition check: `libXcomposite-dev` + runtime `Composite` present; X11
      session; `libXdamage`/`libXfixes` present. `-lXcomposite` not yet linked.
- [x] Design freshness pass complete + committed.
- [x] User additions folded in (2026-07-09): native-res "definition" texture;
      source-window resize scales panel size proportionally at same aspect
      (size model Option 1; angular-DPI → future); **active-screen-only X×Y
      label rendered just outside the panel** — the first on-glasses glyphs
      (`text_raster`, minimal bitmap-digit font); **persistent thin frame on
      all floating-window panels (gray/white, green while selected)**, reusing
      the green-border 4-bar geometry — monitor screens unframed.
- [ ] User review of the reconciled + extended spec.
- [ ] Implementation plan (writing-plans) — TDD, tasks.
- [ ] Implement: WindowBackend → per-source renderer (+label texture) → scene
      source_index + resize size-scale → config keys → `Ctrl+Alt+W` handler +
      spawn → text_raster + active-screen label quad → Makefile → telemetry.
- [ ] Build green + unit tests + X-gated component test.
- [ ] Hardware pass on glasses.
- [ ] Merge to `main` (FF) when finalized.

## Key grounding (from the 2026-07-09 architecture map)

- Capture: `CaptureBackend` (`capture.h:18-36`); ONE active backend via chain
  (`main.cpp:509-562`); each backend self-threaded. XShm reads any Drawable
  (`capture_xshm.cpp:150`) → window pixmap reuses it.
- Renderer: single texture + single dset bound once (`vk_renderer.cpp:548`);
  `flags[0]=textured` gives the placeholder path. → per-source array (N=8),
  per-draw dset bind.
- `ScreenInst{cfg,uv,aspect}` (`scene.h:13-17`); `active_screen` (`main.cpp:643`);
  no runtime add today (`main.cpp:807` "future feature").
- Hotkeys `hot[]` (`main.cpp:429`), handlers (`main.cpp:721-760`) — add `XK_w`.
- Config per-screen whitelist (`config.cpp:104-107`) — add `source`,
  `window-match`.
