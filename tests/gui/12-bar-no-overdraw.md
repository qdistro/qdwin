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

# Best-effort wait for qdshell's WindowManagerService to register the WM
# shortcuts, so the Super+Up chord isn't dropped by lazy registration
# under full-run load (per 19-wm-policy.md). The HARD guard is the
# cursor-scoped set_maximized check below; this only reduces flake.
for _ in $(seq 1 10); do
  "$QDWIN_VM_EXEC" "$VMNAME" \
    "journalctl _UID=1000 --no-pager | grep -q 'registered WM shortcuts'" && break
  sleep 1
done

# Capture a fresh cursor IMMEDIATELY before the chord so the set_maximized
# assertion reads ONLY this chord's effect — never a stale or unrelated
# line. Since the client does not self-maximize, this journal line is the
# sole proof the Super+Up → request_maximize path actually fired.
MAXCURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")

# Maximize via the WM Super+Up shortcut (real-keyboard chord through
# QMP — the same mechanism 04/15/19 use for compositor-side chords).
qdwin_chord meta_l -- up

# Poll (don't single-sleep-race) for THIS chord's set_maximized on our
# handle, scoped to the post-chord cursor.
MAXLINE=""
for _ in $(seq 1 10); do
  sleep 1
  MAXLINE=$("$QDWIN_VM_EXEC" "$VMNAME" \
    "journalctl _UID=1000 --after-cursor='$MAXCURSOR' --no-pager | \
     grep -E 'qdwin: set_maximized handle=$HANDLE max=1' | tail -1")
  [ -n "$MAXLINE" ] && break
done
[ -n "$MAXLINE" ] || { echo "ERROR: Super+Up did not maximize handle=$HANDLE (no set_maximized after the chord — WM shortcut not registered/dispatched?)"; exit 1; }
echo "set_maximized (this run): $MAXLINE"
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
   cat /home/admin/.config/qdshell/settings.json.tmp > \
   /home/admin/.config/qdshell/settings.json && \
   rm -f /home/admin/.config/qdshell/settings.json.tmp'"
# Inode-preserving in-place write (cat > file, NOT mv): a rename-replace
# swaps the inode, and qdshell's FileView/QFileSystemWatcher can miss or
# lag the change by minutes (run 877512 reloaded ~2.5 min late; run
# 1793885 never reloaded in-window → 3.1 false-FAIL). Truncating the SAME
# inode fires IN_MODIFY → FileView watchChanges:true reloads promptly →
# Settings.data.bar.exclusionZoneBleed flips → BarExclusionZone bleedInset
# rebinds → implicitHeight 31→30 → qdwin reconfigures the exclusion zone.
# Poll for that reconfigure (don't single-sleep-race the async reload).
EXCL=""
for _ in $(seq 1 15); do
  sleep 1
  EXCL=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 --no-pager | \
    grep 'qdshell-bar-exclusion-top-Virtual-1' | grep -oE '1920x[0-9]+' | tail -1")
  [ "$EXCL" = "1920x30" ] && break
done
echo "exclusion-top height after bleed=true: ${EXCL:-<none>}"
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
   cat /home/admin/.config/qdshell/settings.json.tmp > \
   /home/admin/.config/qdshell/settings.json && \
   rm -f /home/admin/.config/qdshell/settings.json.tmp'"
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
