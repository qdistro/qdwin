# 19 — window-manager policy live-apply (v25): tiling, fullscreen, focus, hotkeys

**What**: validate the v25 window-manager-policy surface on a live qdwin DRM
session — `set_wm_policy` (focus model / placement / snap), `request_tile`
(half-screen), `request_fullscreen`, and the v19 `register_hotkey` path the
WM keyboard shortcuts ride on. This is the GUI sibling of the headless host
test `tests/host/13-wm-policy.md`; it covers the parts headless can't:
real client resize on tile, fullscreen fill, and (where input can be driven)
focus-follows-mouse + hotkey delivery.

**Why**: v25 is what flips the qdshell WindowManager settings tab from
persist-only to live-apply (`CapabilityService.wmPolicy` /
`keybindRegistration`). The compositor must resize the real client on a tile
(not just move chrome), fill the output on fullscreen, and hold the registered
hotkeys.

## Environment

Standard qdwin GUI harness (`tests/gui/AGENTS.md`): a running libvirt domain
on `qemu:///session` with `noctalia-session.service` (weston + qdwin-shell.so)
and `noctalia-shell.service` (qdshell). Deploy the v25 qdwin-shell.so +
qdshell qml-plugin first (build in-VM, install to `/usr/lib64/weston/` and
`/usr/share/qdistro/qml/Qdistro/Qdwin/`, restart `noctalia-session.service`).

## Path A — qdshell-driven (capability + settings)

1. `journalctl --user -u noctalia-shell.service` shows on bind:
   `CapabilityServ wmPolicy -> true` and `keybindRegistration -> true`
   (the binding negotiated qdwin_shell_v1 v25). **Validated 2026-05-29** on
   `qdistro-daily-2026-05-29`: both flipped true.
2. Open Settings → Window Manager: the persist-only capability banner is
   hidden (canApplyWmPolicy true). Toggle focus policy to "Focus follows
   mouse" → `journalctl -u noctalia-session.service` logs
   `qdwin: set_wm_policy focus=1 …`.
3. Bind a WM shortcut (e.g. tile-left = Super+Left), focus a window, press it:
   the window tiles to the left half.

> Caveat (VM env): `WindowManagerService.init()` runs from shell.qml's
> deferred (`Qt.callLater`) service-init chain. On a VM lacking
> bluetooth/UPower that chain emits warnings; if it is interrupted before the
> WindowManager entry the service is instantiated lazily on first Settings-tab
> open instead. The capability flip (driven from Qdwin.qml on bind) is
> independent and always fires.

## Path B — compositor functional proof (bystander as shell)

Drives the compositor directly on the live DRM session, independent of
qdshell's init: stop `noctalia-shell.service`, bind the v25 `qdwin-bystander`
as the shell, spawn `weston-terminal` (handle 1), drive the FIFO, then restart
`noctalia-shell.service`. (Script: a sibling of `agent-*` smokes.)

```
wmpolicy 1 250 1 0 2 1 24     # follow-mouse, smart placement, snap 24px
tile 1 left                   # left half
tile 1 none                   # restore
tile 1 right                  # right half
fullscreen 1 1                # fill output
fullscreen 1 0                # restore
hotkey 7101 2 62              # Alt+F4
```

**Assert** (compositor journal + bystander `toplevel_geometry`):

- `set_wm_policy focus=1 ffm_delay=250 … placement=2 snap=1 dist=24`
- `tile handle=1 edge=left outer=960x1080 at (0,0)` and the client geometry
  moves to `(0,0)` and **resizes** (not just chrome) — proves
  `apply_inset → set_size` reaches the client.
- `tile handle=1 restored …@(<float-x>,<float-y>)` (pre-tile geometry).
- `tile handle=1 edge=right outer=960x1080 at (960,0)`.
- `set_fullscreen … fs=1 outer=1920x1080 at (0,0)` then `fs=0 restored=…`.
- `register_hotkey id=7101 mods=0x2 key=62`.

**Validated 2026-05-29** on `qdistro-daily-2026-05-29` (1920×1080 DRM):
all of the above logged exactly; the terminal client resized to (0,0) /
(960,0) / full 1920×1080 and restored to its floating rect across the
sequence. qdshell restarted cleanly afterwards.

## Not covered here

- Focus-follows-mouse *retarget delay* and edge-snapping during an interactive
  drag are driveable with `qdwin_mouse_move` / `qdwin_drag` (see
  `qdwin-helpers.sh`) but timing-sensitive; assert via the `seat_focus_changed`
  journal line and the snapped geometry. Hotkey *press delivery* uses
  `qdwin_chord` (real-keyboard sequence) once a shortcut is registered.
