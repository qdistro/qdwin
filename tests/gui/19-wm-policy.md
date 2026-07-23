# 19 — window-manager policy live-apply (v25): capability gate + tile

**What**: validate the v25 window-manager-policy capability surface on a live
qdwin DRM session via the qdshell IPC contract — that the shell binds the
compositor at >= v25 and exposes `wmPolicy` + `keybindRegistration` true (what
flips the qdshell WindowManager settings tab from persist-only to live-apply),
plus one real visual proof that a tile resizes the live client. This is the
qdshell-driven half of the v25 surface; the direct-compositor functional proof
(FIFO-driven `set_wm_policy`/`request_tile`/`request_fullscreen`/`register_hotkey`
against `qdwin-bystander`) lives in its sibling `21-wm-policy-bystander.md`.

**Why**: v25 is what flips the qdshell WindowManager settings tab from
persist-only to live-apply (`CapabilityService.wmPolicy` /
`keybindRegistration`). The IPC capability read is the stable, deterministic
contract; journal strings from `CapabilityService` are diagnostic only.

## Environment

Standard qdwin GUI harness (`tests/gui/AGENTS.md`): a running libvirt domain
on `qemu:///session` with `qdwin-compositor.service` (weston + qdwin-shell.so)
and `qdshell.service` (qdshell). **The session is already fully provisioned by
the GUI gate** — the vendored libweston, qdwin-shell.so, qdshell, and the
qml-plugin are baked into the VM image and the user units are active before the
scenario runs. Do NOT build or deploy anything in-VM; just probe the live
session below. (If a precondition probe fails, that is an ERROR to report, not
a cue to provision.)

This scenario is deliberately lightweight: one deterministic IPC assert plus a
single visual judgment, so it fits the agent budget with room to spare. Write
your `status.txt` PASS as soon as the asserts below hold and STOP — do not run
extra diagnostics or screenshots past the one required tile capture.

## Setup

```bash
source ${QDWIN_REPO}/tests/gui/qdwin-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_session_healthy \
  || { echo "ERROR: qdwin/qdshell user session not up"; exit 1; }

# qs_ipc <method> [args...] — call a qdwin IPC method on the running qdshell
# instance. Same proven-working invocation as 16/17: `runuser -u admin --
# env … WAYLAND_DISPLAY=wayland-1 qs ipc -p PATH call qdwin …`, with a PID
# fallback if the -p path lookup can't find the instance.
QS_PATH=/usr/share/quickshell/qdshell
qs_ipc() {
    local out
    out=$("$QDWIN_VM_EXEC" "$VMNAME" \
        "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 \
         qs ipc -p $QS_PATH call qdwin $*" 2>&1)
    if printf '%s' "$out" | grep -qiE 'no running instance|No such'; then
        local pid
        pid=$("$QDWIN_VM_EXEC" "$VMNAME" \
            "pgrep -u admin -f 'qs -p $QS_PATH' | while read p; do \
               grep -q dbus-run-session /proc/\$p/cmdline 2>/dev/null || { echo \$p; break; }; done")
        [ -n "$pid" ] || { printf '%s\n' "$out"; return 1; }
        out=$("$QDWIN_VM_EXEC" "$VMNAME" \
            "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 \
             qs ipc --pid $pid call qdwin $*" 2>&1)
    fi
    printf '%s\n' "$out"
}
```

## Step 1 — readiness gate + capability assert (deterministic, load-bearing)

Poll the qdshell IPC capability probe until the shell reports a fully-bound
v25+ session, then assert the v25 capability flags. This is the same
readiness-gate + IPC-contract shape the migrated siblings (16/17/18) use; it is
the load-bearing assertion of this scenario.

```bash
CAPS=
for _ in $(seq 1 30); do
    CAPS=$(qs_ipc capabilities)
    ver=$(printf '%s' "$CAPS" | sed -nE 's/.*version=([0-9]+).*/\1/p')
    case "$CAPS" in
        *bound=true*) [ -n "$ver" ] && [ "$ver" -ge 25 ] && break ;;
    esac
    sleep 1
done
echo "capabilities: $CAPS"
```

**Assert (1.1):** `$CAPS` contains `bound=true` and `version=` >= 25
(the deployed build binds at v28). The shell reached a live qdwin binding.

**Assert (1.2):** `$CAPS` contains `wmPolicy=true` AND `keybindRegistration=true`
— the WindowManager settings tab is live-apply, not persist-only. HARD.

If `bound=true` never appears, the qdshell↔qdwin binding is not reachable —
record ERROR (precondition), not a product FAIL.

Before doing any visual assertion, take a quick screenshot and confirm it is
the qdwin graphical session, not a Linux tty/login screen. If the framebuffer
capture is on the wrong VT, record ERROR and stop; a tty screenshot cannot
prove or disprove tiling.

## Step 2 — one visual proof: a tile resizes the live client

Spawn a known test client, drive the default registered tile-left shortcut
(Super+Left), and confirm the real client (not just chrome) moved to the left
half. This is the single required visual judgment; capture exactly one
screenshot for it.

Tile is NOT an IPC method — the WM tiles ride the registered keyboard shortcut
(`keybindRegistration`): WindowManagerService registers `Super+Left → tile-left`
(qdshell default `windowManager.shortcutTileLeft`) once the shell binds at
>= v25, and dispatches it to `Qdwin.requestTileHandle(focusedHandle, 1)`. So we
focus the test window and send the real-keyboard chord `Super+Left` via
`qdwin_chord`.

```bash
"$QDWIN_VM_EXEC" "$VMNAME" 'command -v qdistro-test-window >/dev/null' \
    || { echo "ERROR: qdistro-test-window not installed (cannot drive tile)"; exit 1; }
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -f "[q]distro-test-window" 2>/dev/null; sleep 1' >/dev/null

CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
"$QDWIN_VM_EXEC" "$VMNAME" \
    "setsid -f runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 qdistro-test-window --title 'qd19-tile' \
     --width 400 --height 260 --color 0xff304050 >/tmp/qd19-tile.log 2>&1"
sleep 2
HANDLE=$("$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --after-cursor='$CURSOR' --no-pager | \
   grep -E 'qdwin: toplevel_added handle=[0-9]+ uid=1000 pid=[0-9]+ app_id=qdistro-test-window' | \
   tail -1 | sed -nE 's/.*handle=([0-9]+).*/\1/p'")
[ -n "$HANDLE" ] || { echo "ERROR: test window never mapped (precondition)"; exit 1; }

# The newly-mapped window holds keyboard focus; drive the default registered
# tile-left shortcut. Super = Meta (qcode meta_l). Use qdwin_chord (real-keyboard
# sequence) — a modifier+key chord needs the modifier-release transition, see
# AGENTS.md "Why two key paths".
qdwin_chord meta_l -- left
sleep 1
qdwin_screenshot /tmp/19-tile-left.png
```

**Assert (2.1):** the journal shows `qdwin: tile handle=$HANDLE edge=left`
with the outer geometry at the left half of the output **work area** — the
output minus the top-bar exclusive zone, exactly like maximize (only
`request_fullscreen` covers the full output at `(0,0)`). On this fixed
1280x800 GUI profile the qdshell bar reserves 31px at the top, so the required
line is `outer=640x769 at (0,31)` (left half width 640; height 800−31=769;
origin y=31, below the bar). The screenshot should show the
`qd19-tile` window occupying the left half of the output (the client itself
resized — not just chrome moved). If the journal tile line is present but
the screenshot path is capturing the wrong VT/tty, record that as visual
evidence unavailable and keep Step 2 passing on the deterministic journal
proof. If the registered shortcut can't be driven (e.g. the user changed
`shortcutTileLeft` away from `Super+Left`), record SKIP/ERROR for Step 2
only — Step 1 is the mandatory deterministic gate.

### Cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -f "[q]distro-test-window" 2>/dev/null; true' >/dev/null
```

## Pass criteria

Step 1 (1.1 + 1.2) mandatory and deterministic — this is the v25 capability
contract. Step 2 (2.1) is the single visual proof; SKIP allowed only if no tile
path can be driven. Write `status.txt` PASS once 1.1 + 1.2 hold (and 2.1 passes
or is a justified SKIP) and STOP.

## Known-broken-if

- 1.2 `wmPolicy=false`/`keybindRegistration=false` while `bound=true
  version>=25`: the capability flip on bind didn't fire. Check
  `Services/Qdwin/Qdwin.qml`'s `onBoundChanged` sets
  `CapabilityService.setWmPolicy`/`setKeybindRegistration` on `shellVersion >= 25`.
- 1.1 never reaches `bound=true`: the qml-plugin never bound qdwin_shell_v1.
  Check `qs ipc list` shows the `qdwin` target and the compositor advertises
  the global. Record ERROR, not FAIL.
- 2.1 chrome moves but the client doesn't resize: `apply_inset → set_size`
  didn't reach the client — a real compositor defect (mirror of the headless
  `tests/host/13-wm-policy.md` resize assertion).

## Not covered here

The direct-compositor functional proof (bystander as shell: `set_wm_policy`
focus/placement/snap, `request_tile` left/right/restore, `request_fullscreen`
fill/restore, `register_hotkey`) is exercised in `21-wm-policy-bystander.md`.
Focus-follows-mouse retarget-delay and edge-snapping during an interactive drag
remain out of scope (timing-sensitive).
