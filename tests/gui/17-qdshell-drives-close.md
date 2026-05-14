# 17 — qdshell drives request_close via qdwin_shell_v1

**Acceptance criterion:** qdshell's `Qdwin.closeWindow(window)` (the
Q_INVOKABLE on the `QdwinBinding` exposed by the
`Qdistro.Qdwin` plugin) sends `qdwin_shell_v1.request_close(handle)`,
qdwin honors it, the target toplevel exits cleanly, and a
`toplevel_removed` event propagates back to the shell — closing the
loop end-to-end. Before workstream A, `Services/Qdwin/Qdwin.qml`'s
`closeWindow(window)` was a literal TODO stub.

## Prerequisites

Same as `16-qdshell-binding-protocol-events.md`. Additionally:

- A way to invoke `Qdwin.closeWindow(window)` from outside qs. We use
  the `IPCService` that qdshell already wires for testing
  (`Modules/IPC/...` exposes a generic command channel). Concretely,
  the agent runs:

  ```bash
  qs -p /usr/share/quickshell/qdshell ipc \
      'Qdwin.closeWindow({handle: HANDLE})'
  ```

  inside the admin user session.

Fail loudly if the IPC isn't available; do not skip.

## Setup

```bash
source ${QDWIN_REPO}/tests/gui/qdwin-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_session_healthy || { echo "FAIL: session not up"; exit 1; }

"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     qs -p /usr/share/quickshell/qdshell ipc list 2>&1 | head -3'" \
    | grep -q Qdwin \
    || { echo "FAIL: qs ipc bridge or Qdwin singleton not reachable"; exit 1; }

"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x foot 2>/dev/null; sleep 1' >/dev/null
```

## Steps

### Step 1 — spawn a target toplevel

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 nohup foot sleep 600 >/dev/null 2>&1 &'"
sleep 2
HANDLE=$("$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --no-pager | \
   grep -E 'qdwin: toplevel_added' | tail -1 | \
   sed -nE 's/.*handle=([0-9]+).*/\1/p'")
[ -n "$HANDLE" ] || { echo "FAIL: no toplevel_added handle"; exit 1; }
```

### Step 2 — drive close via QML IPC

```bash
CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     qs -p /usr/share/quickshell/qdshell ipc call \
     Qdwin closeWindow {\"handle\":$HANDLE}'"
sleep 1
```

**Assert (2.1):** a `qdwin: request_close handle=$HANDLE` log line
appears in the journal after `$CURSOR`. This proves the QML side
issued the `qdwin_shell_v1.request_close` request and the
compositor processed it.

**Assert (2.2):** within 2s of the above, `qdwin: toplevel_removed
handle=$HANDLE` appears in the journal — foot exited cleanly in
response to xdg_toplevel.close.

**Assert (2.3):** `qdwin: seat_focus_changed seat=default
handle=4294967295` (or to a surviving sibling) appears within
500ms of `toplevel_removed`, confirming the focus-recovery idle
ran on the shell-driven close path the same way it would for a
user-initiated close.

### Step 3 — Verify no orphan in qdshell windows model

If the agent has visual access, screenshot the bar's
ActiveWindow widget. After the close:

**Assert (3.1):** the bar reflects "no active window" (no title
shown, or a placeholder). Mirrors the
`focusedWindowIndex === -1` state in the QML singleton.

### Cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x foot 2>/dev/null; true' >/dev/null
```

## Pass criteria

2.1, 2.2, 2.3 mandatory. 3.1 soft.

## Known-broken-if

- 2.1 silent: the IPC call reached the QML side but
  `qdwinBinding.closeWindow` didn't fire the request. Likely the
  QML binding's `Q_INVOKABLE void closeWindow(quint32 handle)`
  isn't reachable through the IPC bridge — check that the
  argument marshalling matched (the IPC bridge wraps args in JSON;
  the QML side may need `parseInt`).
- 2.2 fires but 2.1 silent: that's impossible — qdwin can't kill
  a toplevel without an originating request, unless the foot
  process itself exited on its own (the `sleep 600` SIGCHLD would
  fire after 10 minutes). Re-check $HANDLE.
- 2.3 silent: focus-recovery didn't run, or it ran but didn't
  emit `seat_focus_changed`. The latter would imply the binding
  unwound mid-request — check for `qdwin-binding: error:` lines
  in the journal between Step 2 and 3.

## Why agent-driven

The IPC mechanism (`qs ipc call ...`) is the right invocation
shape but the *exact* JSON encoding for the second argument
(`{handle: N}`) depends on Quickshell's IpcHandler conventions —
something an agent can iterate on with `qs ipc list` introspection
faster than a fixed shell script. Step 3 (visual confirmation of
the bar's empty state) also benefits from agent-side
screenshot/OCR. The whole scenario is a contract test for a
new code path; framing it as an exploration that emits a
PASS/FAIL CHECK line is more robust than locking the JSON shape
into a deterministic .bats test on day one.
