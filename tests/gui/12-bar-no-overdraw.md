# 12 — bar bottom row does not overdraw maximized work area

**Acceptance criterion:** the qdshell bar's bottom row (y=30 by default)
does NOT visibly overlap the top row of a maximized client. With the
default `exclusionZoneBleed: false` (since the pixel-mismatch fix in
`todo/qdshell-bar-pixel-mismatch.md`), `bar-content` and
`bar-exclusion-top` agree to the pixel and the maximized window starts
at the row immediately below the bar.

## Setup

```bash
source ${QDWIN_REPO}/tests/gui/qdwin-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"

pgrep -f "http.server 8765" >/dev/null || (
    cd ${QDWIN_REPO} && \
    python3 -m http.server 8765 --bind 127.0.0.1 >/tmp/qdistro-http.log 2>&1 &
)
sleep 1
qdwin_session_healthy || { echo "FAIL: session not up"; exit 1; }

"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x weston-terminal 2>/dev/null; sleep 1' >/dev/null
```

## Steps

### Step 1 — confirm bar-content height == bar-exclusion height

```bash
"$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 --no-pager | \
  grep -E 'qdshell-bar-(content|exclusion-top)-Virtual-1' | tail -4"
```

**Assert (1.1):** both `qdshell-bar-content-Virtual-1` and
`qdshell-bar-exclusion-top-Virtual-1` log lines report the same
`1920xH` height (default density: H=31). If they differ by ≥1, the
fix has regressed — see `Settings.data.bar.exclusionZoneBleed`.

### Step 2 — maximize a base weston-terminal and observe its outer geometry

We use `weston-terminal` (from the base-image `weston` package), NOT
`foot` — `foot` lives in the opt-in `QDWIN_APP_DEPS` lane (off by
default since 5f48e17) and is absent from the lean GUI golden. The
assertion is about the COMPOSITOR's `set_maximized` geometry, not any
specific client, so any maximizable toplevel works. `weston-terminal`
has no `--maximized` flag, so we maximize it through qdwin's own path:
the WM `Super+Up` (toggle-maximize) shortcut, registered by qdshell's
`WindowManagerService` and dispatched via `requestMaximizeHandle` →
`qdwin_shell_v1.request_maximize` → the same `set_maximized` log line.

```bash
# Capture a journal cursor so we read only THIS run's toplevel_added.
CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
# setsid -f detaches the client so it survives the vm-exec shell
# returning (same launch shape as 17-qdshell-drives-close.md).
"$QDWIN_VM_EXEC" "$VMNAME" \
  "setsid -f runuser -u admin -- env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 \
   WAYLAND_DISPLAY=wayland-1 weston-terminal \
   >/tmp/qd12-weston-terminal.log 2>&1" >/dev/null
sleep 2
HANDLE=$("$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --after-cursor='$CURSOR' --no-pager | \
   grep -E 'qdwin: toplevel_added handle=[0-9]+ uid=1000 pid=[0-9]+ app_id=weston-terminal' | \
   tail -1 | sed -nE 's/.*handle=([0-9]+).*/\1/p'")
[ -n "$HANDLE" ] || { echo "ERROR: weston-terminal never mapped (no toplevel_added)"; exit 1; }

# Focus the window so qdshell's WindowManagerService._onHotkey acts on
# it (the toggle-maximize hotkey no-ops when focusedHandle <= 0). Uses
# the proven qs-ipc shape from 17-qdshell-drives-close.md / 19-wm-policy.md.
"$QDWIN_VM_EXEC" "$VMNAME" \
  "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 \
   qs ipc -p /usr/share/quickshell/qdshell call qdwin focusWindow $HANDLE" >/dev/null 2>&1
sleep 1

# Maximize via the WM Super+Up shortcut (real-keyboard chord through
# QMP — the same mechanism 04/15/19 use for compositor-side chords).
qdwin_chord meta_l -- up
sleep 2

"$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --no-pager | grep 'qdwin: set_maximized' | tail -1"
qdwin_screenshot /tmp/12-step2-maximized.png
```

**Assert (2.1):** the `set_maximized` line reports
`outer=1920x1049 at (0,31)` (bar height 31 → work area 1080-31=1049,
window y-origin = 31). **Not** `(0,30)` — that's the overdraw bug.

**Assert (2.2):** the screenshot shows the bar's bottom edge flush
against the maximized weston-terminal's top edge with no visible
bar-content spillover into the terminal's content area. The terminal's
top border / titlebar should be intact, not clipped by 1px.

### Step 3 — toggle `exclusionZoneBleed: true` and confirm the regression

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
  "runuser -u admin -- bash -c 'jq \".bar.exclusionZoneBleed=true\" \
   /home/admin/.config/qdshell/settings.json > \
   /home/admin/.config/qdshell/settings.json.tmp && \
   mv /home/admin/.config/qdshell/settings.json.tmp \
   /home/admin/.config/qdshell/settings.json'"
# qdshell Settings.qml FileView has watchChanges:true → hot-reloads on edit.
sleep 2
"$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 --no-pager | \
  grep -E 'qdshell-bar-exclusion-top-Virtual-1' | tail -1"
```

**Assert (3.1):** with the bleed toggled on, the exclusion-top height
is now 30 (one less than bar-content 31). This confirms the toggle is
wired; reset to false before next test.

### Step 4 — reset to default

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
  "runuser -u admin -- bash -c 'jq \".bar.exclusionZoneBleed=false\" \
   /home/admin/.config/qdshell/settings.json > \
   /home/admin/.config/qdshell/settings.json.tmp && \
   mv /home/admin/.config/qdshell/settings.json.tmp \
   /home/admin/.config/qdshell/settings.json'"
sleep 2
```

## Cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x weston-terminal 2>/dev/null; true' >/dev/null
```

## Pass criteria

Asserts 1.1, 2.1, 2.2, 3.1 pass. Confirms the bar's bottom row no
longer paints into the work area by default and that the
`exclusionZoneBleed` setting reproduces the legacy behavior on demand
(for fractional-scale displays that still need the inset).

## Known-broken-if

- Step 1 shows bar-content=31 and exclusion=30 → fix regressed; either
  the setting default flipped, or `BarExclusionZone.qml` lost the
  conditional. Diff against
  `qdshell/Modules/MainScreen/BarExclusionZone.qml`.
- Step 2 shows `outer=1920x1050 at (0,30)` → work-area math in qdwin
  is still subtracting the bleed instead of the full bar. Check
  `qdwin_compute_work_area` (or the equivalent panel helper) in
  `qdwin/qdwin.c`.
- Step 3 has no effect → settings file or reload pipeline isn't
  picking up `exclusionZoneBleed`. Verify the property landed in
  `qdshell/Commons/Settings.qml`.
