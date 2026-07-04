# M3 remainder — glasses-on verification (handoff)

Build: `cd spatial-screens && make`. Stop `viture-bridge` first. Launch only
via `./run.sh`. Never double-start. After any hard kill in direct mode:
`xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`.

## Panic / recovery

- If the whole display stack hangs (all outputs black, VT switch dead —
  NVIDIA can hang on the idle-blank DPMS modeset on this hybrid GPU box): the
  machine usually stays alive on the network. Recover via SSH (tailscale
  host, e.g. `ssh sonmorri`) and `sudo systemctl restart gdm` rather than a
  power cut — it preserves logs. Only hard-power-cycle if SSH is also dead.
- Before a glasses-on session, inhibit idle blanking
  (`gnome-session-inhibit --inhibit idle <cmd>`, or disable auto screen-blank
  in Settings) — the hang above triggered on the idle blank a few minutes
  after the hands left the keyboard, not during active use.

- [x] 1. `./run.sh --pitch-trim 16` — portal picker appears once; pick the
      laptop monitor. Virtual screen shows that monitor live, cursor visible,
      ~118–120 fps in direct mode. Console says `capture: portal`.
      PASSED 2026-07-04 with one finding: the cursor was NOT in the portal
      frames (mutter on X11 ignores the embedded cursor_mode) — fixed
      forward with an XFixes overlay blended each capture tick.
- [x] 2. Quit (Ctrl+Alt+Q), relaunch — NO picker dialog (restore token from
      `~/.local/state/spatial-screens/state`), same monitor streams.
- [x] 3. Live-tune with hotkeys (Ctrl+Alt+[ ] - =), quit, relaunch — tuned
      distance/size persist. Then `--distance 0.75` on the CLI overrides them.
      PASSED (distance 1.143 and size 40 restored across runs; CLI won).
      Finding fixed forward: the size hotkey floor was 40" (default is 24,
      so sizing back down was impossible) — now 10".
- [x] 4. `--capture-backend xshm` — behavior parity with the pre-M3 build
      (monitor content, resize/reflow survival via RandR events).
      PASSED: content + resize survival (2560x1600→1920x1200→back under the
      app; fps dipped to 65 during the modeset, recovered). Cursor now
      present on xshm too (new overlay). Note: at capture-hz 120 the
      synchronous XShm grabs cost render fps (~110-115); ~60 is a better
      fit for this backend.
- [x] 5. Fallback: `systemctl --user stop xdg-desktop-portal` then launch —
      console shows portal failure → `capture: xshm`, app still works.
      Restart the portal service afterwards.
      PASSED, with a premise correction: `stop` alone is defeated by D-Bus
      activation (the ScreenCast call restarts the service — the app then
      just uses portal normally, itself a resilience win). Forcing failure
      via `systemctl --user mask --runtime` gave the real test: instant
      CreateSession failure → "falling back" → `capture: xshm`, no hang.
      Unmasked + restarted afterwards.
- [x] 6. Dashboard: `cd sensor-viz && npm run dev`, open http://localhost:5173,
      Connect Bridge while spatial-screens runs → device info + live pose in
      the 3D view + "spatial-screens" panel (fps ≈118, 6DoF LIVE, distance/
      size/backend/direct correct, values track hotkey changes within ~1 s).
      PASSED (user-verified; fps readout runs 110-119 with the 120 Hz
      capture tick — see item 4 note).
- [ ] 7. Dashboard Recenter button → screen re-places in front of you; event
      log shows "pose reset via dashboard".
      OPEN — two real bugs found and fixed during the pass (the app used
      the stale pre-reset pose after the VIO reset — now re-seeds and
      re-places ~0.5 s after; and the dashboard button never sent
      reset_pose over the WS at all — now wired to BridgeClient.resetPose
      with local offsets cleared), but the end-to-end still failed on
      hardware: "pose reset via dashboard" never appeared in the app
      console, so the request still isn't arriving. Next step: inspect the
      WS frames in browser devtools (stale page vs. connection state).
      Hotkey/gesture recenters work throughout.
- [x] 8. Gestures still work (pinch-drag distance, fist-hold recenter) — the
      capture refactor must not disturb the camera callback path.
      PASSED (dozens of fist-hold recenters + pinch-drag distance ramps
      0.5→3.5 m in the run logs).
- [x] 9. Kill -9 the app mid-run → relaunch works; portal session from the
      dead run doesn't wedge the new one (GNOME may show a stale sharing
      indicator until the next session — cosmetic only).
      PASSED: relaunch streamed instantly off the restore token. The manual
      DP-1 recovery documented above WAS required — and the output also
      lost its mode (came back 1920x1080@60+0+0), so the full recovery is:
      `--set non-desktop 0`, then `--mode 1920x1200 --rate 120 --pos 322x0`.
      Backlog: the app could salvage a stranded non-desktop output at
      startup (pairs with the single-instance-guard follow-up).
- [ ] 10. Memory watchdog / leak watch — run direct mode for 10+ min and
      watch the dashboard's Memory (rss) row and the console. RSS should
      plateau, not climb steadily. (A prior run leaked to ~15.9 GB and was
      OOM-killed.) If it climbs: note the rate, whether it correlates with
      `6dof frozen` episodes, and A/B it with the gesture sidecar disabled
      and with `--capture-backend test` (isolates capture vs SDK vs gesture
      as the leak source). The app now self-shuts-down gracefully above 8 GB
      to avoid an OOM SIGKILL stranding the display.
- [x] 11. Portal cold-grant stall — first-ever launch (no stored restore
      token) shows the GNOME screen-share picker and blocks up to 120s
      waiting for your choice; once granted, the token is stored and
      subsequent launches are instant with no dialog. If the picker is
      dismissed/times out, the app falls back to xshm. Verify: fresh launch
      → pick monitor → quit → relaunch shows NO dialog.
      PASSED (state moved aside to force the cold path; grant → token →
      instant relaunches all confirmed). Dismiss/timeout→xshm not yet
      exercised; the portal-stop fallback is item 5.
