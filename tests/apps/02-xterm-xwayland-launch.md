# 02 — xterm launches without crashing weston

**Acceptance criterion:** spawning an XWayland client (xterm) does not
SIGSEGV weston. Regression-tests qdwin bug #2 (NULL `wl_client` from
`weston_desktop_client_get_client` for XWayland-spawned surfaces was
dereferenced inside `qdwin_client_uid`).

## Setup

```bash
source phase1/gui-tests/qdwin-apps/qdwin-apps-helpers.sh
qdwin_apps_set_vm "${VMNAME}"
qdwin_apps_session_up || { echo "FAIL: bystander/weston not healthy"; exit 1; }
qdwin_apps_kill_all

# Snapshot weston pid before so we can detect a restart.
WESTON_PID_BEFORE=$(virsh qemu-agent-command "$VMNAME" \
    '{"execute":"guest-exec","arguments":{"path":"/bin/sh","arg":["-c","pgrep -u admin weston | head -1"],"capture-output":true}}' \
    2>/dev/null | head -1 || true)
# fallback via vm-exec
WESTON_PID_BEFORE=$(qdwin_apps_log_grep 'WESTON_PID' 2>/dev/null || \
    "$QDWIN_VM_EXEC" "$VMNAME" 'pgrep -u admin weston | head -1' 2>/dev/null)
echo "weston pid before: $WESTON_PID_BEFORE"
```

## Steps

### Step 1 — launch xterm

```bash
qdwin_apps_launch xterm "xterm -fa Monospace -fs 12"
sleep 6
qdwin_apps_screenshot /tmp/02-step1-xterm.png
```

**Assert (1.1):** screenshot shows xterm rendered with XWayland CSD
(grey title bar with min/max/close buttons, white content area, shell
prompt visible — `admin@…> ` or similar).
**Assert (1.2):** bystander log shows
`toplevel_added handle=<N> ... title="xterm" xwayland=1`. Note the
`xwayland=1` — that's the bug #3 fix for free.
**Assert (1.3):** weston pid did NOT change between Setup and now —
proving qdwin didn't crash. Use:

```bash
WESTON_PID_AFTER=$("$QDWIN_VM_EXEC" "$VMNAME" 'pgrep -u admin weston | head -1')
[ "$WESTON_PID_BEFORE" = "$WESTON_PID_AFTER" ] || \
    echo "FAIL: weston restarted (pid $WESTON_PID_BEFORE → $WESTON_PID_AFTER)"
```

### Step 2 — type into xterm

```bash
qdwin_apps_send_key KEY_E KEY_C KEY_H KEY_O KEY_SPACE KEY_Q KEY_D KEY_W KEY_I KEY_N KEY_ENTER
sleep 1
qdwin_apps_screenshot /tmp/02-step2-typed.png
```

**Assert (2.1):** screenshot shows the typed command on one line and
output on the next. Tests that wl_keyboard delivery to XWayland
works after the NULL-client fix (the SIGSEGV used to fire during
`toplevel_added`, before the keyboard path was ever exercised).

## Cleanup

```bash
qdwin_apps_ctl "close" || qdwin_apps_kill_all
```

## Pass criteria

- xterm window visible in step 1 screenshot.
- weston PID unchanged (no SIGSEGV restart).
- bystander log records `xwayland=1` (proves bug #3 fix is also live).

## Known failure modes

- **Pre-fix regression (bug #2)** — xterm shows "X connection to :0
  broken (explicit kill or server shutdown)" in `/tmp/xterm.log`,
  weston pid changed, qdwin.log shows no `toplevel_added`. The
  `qdwin_client_uid` NULL guard at `qdwin.c:679` regressed.
- **Xwayland not installed** — qdwin.log line "launching '/usr/bin/Xwayland'"
  immediately followed by "exited with status 1". Need
  `zypper -n install xwayland`. Also see
   item 1.
