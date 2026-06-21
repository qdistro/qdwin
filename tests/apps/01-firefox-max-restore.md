# 01 — Firefox round-trips request_maximize → request_maximize(0)

**Acceptance criterion:** after a `max` then `restore` via the bystander
FIFO, Firefox is back at (approximately) its pre-max geometry — not
sticking at the maximised dimensions. Regression-tests qdwin bug #1
(saved `outer_*` was 0 before first `apply_inset`).

## Setup

```bash
source phase1/gui-tests/qdwin-apps/qdwin-apps-helpers.sh
qdwin_apps_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_apps_session_up || { echo "FAIL: bystander/weston not healthy"; exit 1; }
qdwin_apps_kill_all
```

## Steps

### Step 1 — launch firefox

```bash
qdwin_apps_launch firefox "firefox --no-remote --new-instance about:blank"
sleep 12
qdwin_apps_screenshot /tmp/01-step1-launched.png
```

**Assert (1.1):** screenshot shows the Firefox window with tabs and
URL bar visible. Window does NOT fill the screen — there should be
black margin on at least one edge.
**Assert (1.2):** bystander log line for this step contains
`toplevel_added handle=<N> app_id="firefox"`. (Verify via
`qdwin_apps_log_grep 'toplevel_added.*firefox'`.)

### Step 2 — maximise via shell

```bash
qdwin_apps_ctl "maxlast"
sleep 2
qdwin_apps_screenshot /tmp/01-step2-max.png
```

**Assert (2.1):** screenshot shows Firefox filling the entire 1280×800
output. No black margin on any edge. Tabs/URL bar at the very top.
**Assert (2.2):** bystander log shows `cmd max handle=<N>` followed by
`toplevel_state handle=<N> state=0x1` (bit 0 = maximised).

### Step 3 — restore via shell

```bash
qdwin_apps_ctl "restorelast"
sleep 2
qdwin_apps_screenshot /tmp/01-step3-restored.png
```

**Assert (3.1):** screenshot shows Firefox back at approximately its
launch size, NOT at the maximised dimensions. Black margin on some
edge again. Allow ±20 px wobble; absolute pixel match isn't required.
**Assert (3.2):** bystander log shows `cmd restore handle=<N>` then
`toplevel_state handle=<N> state=0x0`.
**Assert (3.3):** the restored window must NOT extend to the right or
bottom edge of the 1280×800 screen — that's the visual signature of
the pre-fix bug (saved geometry was 0 → restore picked up the
just-committed maximised size).

## Cleanup

```bash
qdwin_apps_ctl "close" || qdwin_apps_kill_all
```

## Pass criteria

- All three screenshots match assertions.
- The width/height of the window in step 3 must be within 20 px of step 1
  (i.e. restore worked).

## Known failure modes

- **Pre-fix regression (bug #1)** — step 3 shows Firefox at 1150×780
  (or near-maximised dimensions) at saved x/y. Means the
  `qdwin_handle_request_maximize` fix at `qdwin.c:1534` regressed.
- **Window decoration drift on Firefox restart** — Firefox occasionally
  remembers the maximised state across launches. If step 1 already
  shows Firefox maximised, the test is meaningless. Delete
  `~/.mozilla/firefox/*.default*/sessionstore.*` in Setup if needed.
