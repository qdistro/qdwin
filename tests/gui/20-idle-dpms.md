# 20 — idle / DPMS live apply (v26): display power + ext-idle-notify

**What**: validate the v26 idle/DPMS path on a live qdwin DRM session — the
`set_display_power` request (DPMS all outputs off/on) and the capability gate
that the qdshell Power tab's inactivity-action + display-off timers ride on
(driven by the standard `ext-idle-notify-v1`).

**Why**: v26 flips the Power tab's idle policy from persist-only to live
(`CapabilityService.idleDpms`). The shell owns the idle *timing*
(ext-idle-notify) and the inactivity *action* (suspend/lock — a session
decision); the compositor only enacts display power via `set_display_power`.

## Environment

Standard qdwin GUI harness. Deploy v26 qdwin-shell.so + the qdshell qml-plugin
(which now also binds `ext_idle_notifier_v1` + a `wl_seat` as a client), then
restart `noctalia-session.service`.

## Path A — capability gate

`journalctl --user -u noctalia-shell.service` shows on bind:
`CapabilityServ idleDpms -> true`. This single line proves all three
requirements held in the real session: a >= v26 shell bind (set_display_power),
`ext_idle_notifier_v1` bound, and a `wl_seat` bound — i.e. the qml-plugin's
ext-idle-notify client connected successfully.

**Validated 2026-05-29** on `qdistro-daily-2026-05-29`: `idleDpms -> true`.

## Path B — compositor functional proof (bystander as shell)

Stop `noctalia-shell.service`, bind the v26 `qdwin-bystander` as the shell,
drive the FIFO, restart `noctalia-shell.service`:

```
displaypower 0     # DPMS all outputs off
displaypower 1     # DPMS all outputs on
```

**Assert** (compositor journal):

- `qdwin: set_display_power on=0 (N output…)` then `on=1 (N output…)`.
- No protocol error (bystander bound at v26).

**Validated 2026-05-29** on the 1920×1080 DRM VM: `set_display_power on=0
(1 output)` / `on=1 (1 output)` — the real (virtual) output was power-cycled;
qdshell restarted cleanly afterwards.

## Not covered here

The full idle *trigger* (wait N minutes → `idled` → action / DPMS-off) is
timing-bound; to exercise it quickly set a short `power.inactivityTimeout*` /
`power.displayOff*` (the UI is in minutes) and watch the
`PowerService` "idle policy armed" + "display-off idle -> DPMS off" journal
lines, then move the pointer (`qdwin_mouse_move`) to fire `resumed` →
`set_display_power(1)`. Presentation mode (`IdleInhibitorService`) must
suppress both — verify no DPMS-off fires while it is active.
