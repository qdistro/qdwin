# 18 — workspaces: switch hides/shows windows; count change (v24)

**Acceptance criterion:** with real qdwin workspaces (v24, ext-workspace-v1
+ the qdshell `workspace` IPC), a window spawned on the active workspace
disappears when the user switches to another workspace and reappears on
switching back; a window spawned while on workspace 2 is visible there and
hidden on workspace 1. Also: changing the workspace count from qdshell
settings reconciles the compositor's workspace count. This is the
user-facing behaviour the settings UI promised
(`todo/issues/qdshell/qdshell-workspaces-and-appearance-settings.md`);
before v24 `Qdwin.switchToWorkspace()` was a no-op.

## Prerequisites

- A qdwin + qdshell session (same as `16`/`17`).
- The qdshell `workspace` IpcHandler (`Services/Control/IPCService.qml`),
  reachable via:

  ```bash
  qs -p /usr/share/quickshell/qdshell ipc --any-display call workspace list
  # → "count=4 active=0 occupied=..."
  ```

  Fail loudly if `qs ipc list` does not show a `workspace` target; do not
  skip.

## Setup

```bash
source ${QDWIN_REPO}/tests/gui/qdwin-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_session_healthy || { echo "FAIL: session not up"; exit 1; }

QS="qs -p /usr/share/quickshell/qdshell ipc --any-display"
qsipc() { "$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 $QS $*'"; }

qsipc list 2>&1 | grep -q workspace \
    || { echo "FAIL: qdshell 'workspace' IPC target not reachable"; exit 1; }

"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x foot 2>/dev/null; sleep 1' >/dev/null
# Start on workspace 0.
qsipc call workspace activate 0 >/dev/null
sleep 0.5
```

## Steps

### Step 1 — spawn FOOT-A on workspace 0, label it

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 nohup foot >/dev/null 2>&1 &'"
sleep 2
qdwin_type_lower "echo i am on workspace zero"
qdwin_send_key KEY_ENTER
sleep 1
qdwin_screenshot /tmp/18-step1-ws0.png
```

**Assert (1.1):** screenshot shows `i am on workspace zero` in foot.
**Assert (1.2):** `qsipc call workspace list` reports `active=0` and
`occupied` includes `0`.

### Step 2 — switch to workspace 1: FOOT-A must vanish

```bash
qsipc call workspace activate 1 >/dev/null
sleep 1
qdwin_screenshot /tmp/18-step2-ws1-empty.png
```

**Assert (2.1):** `qsipc call workspace list` reports `active=1`.
**Assert (2.2):** screenshot shows an **empty** desktop — FOOT-A's
`i am on workspace zero` text is NOT visible (the window was parked on the
hidden workspace layer). HARD — this is the core hide behaviour.
**Assert (2.3):** the journal shows `qdwin: active_workspace=1/` after the
switch.

### Step 3 — spawn FOOT-B on workspace 1, label it

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 nohup foot >/dev/null 2>&1 &'"
sleep 2
qdwin_type_lower "echo i am on workspace one"
qdwin_send_key KEY_ENTER
sleep 1
qdwin_screenshot /tmp/18-step3-ws1-footb.png
```

**Assert (3.1):** screenshot shows `i am on workspace one` (FOOT-B is
visible + focused on workspace 1).
**Assert (3.2):** `i am on workspace zero` is still NOT visible.

### Step 4 — switch back to workspace 0: FOOT-A returns, FOOT-B hides

```bash
qsipc call workspace activate 0 >/dev/null
sleep 1
qdwin_screenshot /tmp/18-step4-back-ws0.png
```

**Assert (4.1):** screenshot shows `i am on workspace zero` again (FOOT-A
restored from the hidden layer).
**Assert (4.2):** `i am on workspace one` is NOT visible.
**Assert (4.3):** `qsipc call workspace list` reports `active=0`.

### Step 5 — `next`/`prev` cycle wraps

```bash
qsipc call workspace next >/dev/null; sleep 0.5
A1=$(qsipc call workspace active)
qsipc call workspace prev >/dev/null; sleep 0.5
A2=$(qsipc call workspace active)
```

**Assert (5.1):** `A1 == 1` (next from 0).
**Assert (5.2):** `A2 == 0` (prev wraps back).

### Step 6 — settings count reconciles to the compositor

```bash
# Raise the workspace count via the settings model. The exact IPC/QML
# path may be `qs ipc call settings ...` then editing workspaces.count,
# or driving Settings directly — the agent introspects with `qs ipc list`.
# Goal: set workspaces.count = 6 and confirm the compositor follows.
qsipc call settings openTab appearance >/dev/null 2>&1 || true
# (agent: set the workspace-count spinner to 6 in the Appearance tab, or
#  poke Settings.data.workspaces.count directly if a test hook exists)
sleep 1
qsipc call workspace count
```

**Assert (6.1):** after raising the setting to 6, `qsipc call workspace
count` reports `6` — qdshell pushed the new count down via
`setWorkspaceCount` → ext-workspace `create_workspace`. SOFT (depends on
the agent being able to drive the spinner; if not reachable, record SKIP
for Step 6 only, not the whole scenario).

## Cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x foot 2>/dev/null; true' >/dev/null
qsipc call workspace activate 0 >/dev/null 2>&1 || true
```

## Pass criteria

Steps 1–5 mandatory (the hide/show + active-index + cycle behaviour).
Step 6 soft (count reconciliation; SKIP allowed if the spinner can't be
driven headlessly).

## Known-broken-if

- 2.2 still shows FOOT-A → the switch didn't hide the window. Check
  `qdwin_set_active_workspace` → `qdwin_toplevel_apply_workspace_visibility`
  moves off-workspace views to `workspace_hidden_layer`.
- 2.1 `active=1` but 2.2 shows FOOT-A → the active index advanced but the
  view wasn't re-parked; suspect the layer move or a repaint not scheduled.
- 4.1 FOOT-A doesn't return → restore path broken (apply_visibility didn't
  move it back to `normal_layer`), or focus recovery picked nothing.
- `workspace list` not found in `qs ipc list` → the IpcHandler didn't
  register; check `Services/Control/IPCService.qml` is loaded and the
  `target: "workspace"` handler compiles.
```
