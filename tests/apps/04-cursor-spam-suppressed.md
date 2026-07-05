# 04 — install_default_cursor warning logs at most once per session

**Acceptance criterion:** the `qdwin: install_default_cursor: no
surface yet` log line fires at most once per qdwin session, not on
every pointer focus change. Regression-tests qdwin bug #4 (one-shot
flag added in `qdwin_install_default_cursor_on_pointer`).

## Setup

```bash
source ${QDWIN_REPO}/tests/apps/qdwin-apps-helpers.sh
qdwin_apps_set_vm "${VMNAME}"
qdwin_apps_session_up || { echo "FAIL: bystander/weston not healthy"; exit 1; }
qdwin_apps_kill_all
```

## Steps

### Step 1 — count pre-existing log lines

```bash
COUNT_BEFORE=$("$QDWIN_VM_EXEC" "$VMNAME" \
    "grep -c 'install_default_cursor: no surface yet' /home/admin/.local/share/qdwin.log || echo 0")
echo "before: $COUNT_BEFORE"
```

### Step 2 — exercise pointer focus changes

Launch and close several apps in sequence to provoke focus change
events (each toplevel arrival/destruction can trigger a default-cursor
install attempt).

```bash
for app in foot foot foot; do
    qdwin_apps_launch "$app" "$app"
    sleep 2
    qdwin_apps_kill_all
    sleep 1
done
qdwin_apps_launch xterm "xterm"
sleep 2
qdwin_apps_kill_all
sleep 1
```

### Step 3 — count post lines

```bash
COUNT_AFTER=$("$QDWIN_VM_EXEC" "$VMNAME" \
    "grep -c 'install_default_cursor: no surface yet' /home/admin/.local/share/qdwin.log || echo 0")
echo "after: $COUNT_AFTER"
```

**Assert (3.1):** `COUNT_AFTER - COUNT_BEFORE <= 1`. The fix lets the
warning fire once per session and then suppresses; without the fix
each app cycle adds 5–20 lines.
**Assert (3.2):** if the warning fired at all, the message must
include the suppression hint:
`(helper not started?) — further occurrences suppressed`. Grep for
that exact string in the qdwin log.

## Cleanup

```bash
qdwin_apps_kill_all
```

## Pass criteria

- Net new lines ≤ 1.
- The single line (if any) carries the "suppressed" suffix.

## Known failure modes

- **Pre-fix regression (bug #4)** — `COUNT_AFTER - COUNT_BEFORE` is
  in the dozens or hundreds. The one-shot flag at
  `qdwin_install_default_cursor_on_pointer` regressed.
- **qdistro-cursor-sprites helper running** — the warning never fires
  at all (helper installed the default sprite). That's also a pass,
  but not what we're testing. Confirm via
  `pgrep -af qdistro-cursor-sprites` returning nothing.
