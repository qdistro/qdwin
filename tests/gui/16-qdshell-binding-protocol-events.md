# 16 — qdshell binds qdwin_shell_v1 and observes protocol events

**Acceptance criterion:** qdshell (via the `Qdistro.Qdwin` QML plugin
that loads `libqdistro-qdwin.so` at startup) binds `qdwin_shell_v1`
at version >= 14, calls `bind_as_shell`, and observes the full
protocol-event stream: `hello`, every `toplevel_added` /
`toplevel_removed`, and every `seat_focus_changed`. The QML side's
`Qdwin` singleton's `windows` ListModel matches qdwin's view of
live toplevels at all times — no extra rows, no missing rows, no
stale titles.

This is the deepest end-to-end assertion that workstream A (the
qdshell ↔ qdwin_shell_v1 binding, landed 2026-05-14) is functional
in both directions: events go server → client AND the client
materially uses them to maintain state.

## Prerequisites

- qdshell built with `meson compile -C build` so
  `qdshell/build/qml-plugin/libqdistro-qdwin.so` exists.
- `install-qdwin-session-for-vm.sh` has installed the plugin to
  `/usr/share/qdistro/qml/Qdistro/Qdwin/` and dropped in the
  `noctalia-shell.service` `QML_IMPORT_PATH` Environment. Without
  these, the QML import fails silently and qdshell runs with the
  no-binding fallback stubs.
- `qs` / `noctalia-qs` 0.0.12+ installed on the VM.
- `qdistro-test-window` on the VM PATH (the same reliable test client
  the qdwin SMOKES use; it deterministically produces a qdwin toplevel
  with `app_id=qdistro-test-window`). This scenario uses it instead of
  `foot`, which is not installed on the VM and so cannot exercise the
  protocol-event paths.

Fail loudly if any prereq is missing; do not skip.

## Setup

```bash
source ${QDWIN_REPO}/tests/gui/qdwin-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_session_healthy || { echo "FAIL: session not up"; exit 1; }

"$QDWIN_VM_EXEC" "$VMNAME" 'test -f /usr/share/qdistro/qml/Qdistro/Qdwin/libqdistro-qdwin.so' \
    || { echo "FAIL: QML plugin not installed on VM"; exit 1; }

# PRECONDITION (infra): the test client must be installed. Absence is an
# ERROR (the scenario cannot be exercised), NOT a product FAIL.
"$QDWIN_VM_EXEC" "$VMNAME" 'command -v qdistro-test-window >/dev/null' \
    || { echo "ERROR: qdistro-test-window not installed on VM (cannot exercise protocol events)"; exit 1; }

"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -f "[q]distro-test-window" 2>/dev/null; sleep 1' >/dev/null
```

## Steps

### Step 1 — bind handshake visible in journal

Restart qdshell so the bind happens after our cursor:

```bash
CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     systemctl --user restart noctalia-shell.service'"
sleep 8
```

**Assert (1.1):** `qdwin: bind accepted for uid=1000` appears in
the journal after `$CURSOR`.

**Assert (1.2):** `INFO qml: ... Qdwin qdwin_shell_v1 bound v14`
appears in the journal after `$CURSOR`. (Both lines must appear —
qdwin's confirms the server saw the request, the QML one confirms
the plugin loaded and dispatched the `hello` event up to QML.)

### Step 2 — toplevel_added reaches QML

```bash
CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
# setsid -f detaches the client into its own session so it survives the
# vm_exec shell returning (a bare `&` gets SIGHUP'd and the toplevel
# never maps). Same launch pattern the qdwin smokes use.
"$QDWIN_VM_EXEC" "$VMNAME" \
    "setsid -f runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 qdistro-test-window --title 'qd16-step2' \
     --width 300 --height 180 --color 0xff304050 >/tmp/qd16-step2.log 2>&1"
sleep 2

HANDLE=$("$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --after-cursor='$CURSOR' --no-pager | \
   grep -E 'qdwin: toplevel_added handle=[0-9]+ uid=1000 pid=[0-9]+ app_id=qdistro-test-window' | tail -1 | \
   sed -nE 's/.*handle=([0-9]+).*/\1/p'")
echo "subject handle=$HANDLE"
```

**Assert (2.1):** $HANDLE is a number (qdwin's `toplevel_added
handle=<N> uid=1000 pid=<N> app_id=qdistro-test-window` log line
fires).

**Assert (2.2):** the `qdwin: seat_focus_changed seat=default
handle=$HANDLE` line is present after `$CURSOR`. This line is
gated behind "a v14+ shell is bound" — it is the load-bearing
proof that the binding is exercising the protocol-emit branch
(prior to workstream A, this line was absent on a toplevel spawn,
even though the underlying `qdwin: focus handle=` ground-truth
line still fired).

### Step 3 — focused window visible in shell UI

The qdshell bar's ActiveWindow widget reads
`Qdwin.getFocusedWindowTitle()`. With the binding in place,
spawning the test window should make the bar reflect its title.

```bash
sleep 1
qdwin_screenshot /tmp/16-step3-window-focused.png
```

**Assert (3.1):** the screenshot's top-bar region (y in [0, 31])
contains the substring "qd16-step2" (the spawned window's title) —
visible because the
`ActiveWindow` widget pulls from `Qdwin.windows`'s focused row.
Use `tesseract` OCR or a `convert -crop` + grep on the rendered
text. Failure here is informative: the QML side received
toplevel_added (asserted in Step 2) but did not surface it,
suggesting the ListModel-append path in `Services/Qdwin/Qdwin.qml`
onToplevelAdded is broken.

### Step 4 — toplevel_removed reaches QML and clears focus

```bash
CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -f "[q]distro-test-window --title qd16-step2"'
sleep 1
```

**Assert (4.1):** `qdwin: toplevel_removed handle=$HANDLE` after
`$CURSOR`.

**Assert (4.2):** `qdwin: seat_focus_changed seat=default
handle=4294967295` after `$CURSOR` — the focus drops to UINT32_MAX
because no toplevel remains. The compositor side runs the focus-
recovery idle (todo/qdwin-focus-events.md) and finds no candidate.

### Step 5 — Alt+Tab cycles focus via the protocol

Spawn two toplevels so Alt+Tab has somewhere to go. The shell's
`onSwitcherNext` + `onSwitcherCommit` handlers (added 2026-05-14
in `qdshell/Services/Qdwin/Qdwin.qml`) walk the windows ListModel
and call `qdwinBinding.focusWindow(handle)` on commit.

```bash
# Capture a cursor BEFORE spawning so we can HARD-GATE on both toplevels
# reaching qdwin — an ungated spawn would leave the Alt+Tab assertions
# depending on windows that may never have mapped (the original failure mode).
SPAWN_CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
"$QDWIN_VM_EXEC" "$VMNAME" \
    "setsid -f runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 qdistro-test-window --title 'qd16-step5a' \
     --width 300 --height 180 --color 0xff304050 >/tmp/qd16-step5a.log 2>&1"
sleep 1
"$QDWIN_VM_EXEC" "$VMNAME" \
    "setsid -f runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 qdistro-test-window --title 'qd16-step5b' \
     --width 300 --height 180 --color 0xff405060 >/tmp/qd16-step5b.log 2>&1"
# Both qdistro-test-window toplevels must reach qdwin before we exercise
# Alt+Tab; otherwise this is a precondition ERROR, not a switcher FAIL.
step5_ok=
for _ in $(seq 1 40); do
    n=$("$QDWIN_VM_EXEC" "$VMNAME" \
        "journalctl _UID=1000 --after-cursor='$SPAWN_CURSOR' --no-pager | \
         grep -cE 'qdwin: toplevel_added handle=[0-9]+ uid=1000 pid=[0-9]+ app_id=qdistro-test-window'")
    [ "${n:-0}" -ge 2 ] && { step5_ok=1; break; }
    sleep 0.2
done
[ -n "$step5_ok" ] || { echo "ERROR: Step 5 — fewer than 2 qdistro-test-window toplevels reached qdwin (got ${n:-0})"; exit 1; }
CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
qdwin_chord alt -- tab
sleep 1
```

**Assert (5.1):** at least one `qdwin: switcher_next dir=` line
after `$CURSOR` (qdwin saw the Tab while Alt was held and emitted
the protocol event to the bound shell).

**Assert (5.2):** at least one `qdwin: focus handle=` line after
`$CURSOR` — confirming the shell's `onSwitcherCommit` actually
called `set_keyboard_focus` and qdwin honored it. Before the
2026-05-14 alt+tab implementation in Qdwin.qml, qdwin would emit
`switcher_next` but the shell did nothing — the focus stayed put.

### Cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -f "[q]distro-test-window" 2>/dev/null; true' >/dev/null
```

## Pass criteria

Asserts 1.1, 1.2, 2.1, 2.2, 4.1, 4.2, 5.1, 5.2 pass. 3.1 is a
soft assertion — useful diagnostic but not blocking (OCR can flake
on antialiasing). If 3.1 fails alone, the binding is fine but the
QML-side UI consumption has a separate issue worth filing.

## Known-broken-if

- 1.1 silent: qdwin saw a registry global_bind but no
  bind_as_shell follow-up. Plugin loaded, called registry_bind
  but raced/segfaulted before bind_as_shell. Check qs stderr for
  segfault.
- 1.2 silent: plugin .so isn't on QML_IMPORT_PATH (check the
  noctalia-shell.service Environment), OR the plugin loaded but
  the QdwinBinding constructor errored before `hello`. Check
  for `qdwin-binding: error:` lines in journal.
- 5.1 silent + 5.2 silent: qdwin's switcher_grab never entered.
  QMP `alt down → tab tap → alt up` injection didn't reach the
  keyboard focus owner. Compare against
  `t_focus_event_on_alt_tab` in `gui-regression-tests.sh` which
  uses the same injection path.
- 5.1 fires but 5.2 silent: the binding is up but Qdwin.qml's
  `onSwitcherCommit` didn't act. Inspect `Services/Qdwin/Qdwin.qml`
  for the alt+tab cycle implementation.

## Why agent-driven

The journal-line asserts are tractable from a plain shell
runner, but step 3 (bar OCR) and the cross-correlation between
qdwin's view of `toplevel_*` events and qdshell's view of its
`windows` ListModel benefit from an LLM able to inspect QML state
via screenshot + diff and decide if the surfaces actually
correspond. A subagent invoked through `gui-regression-tests.sh`'s
`t_agent_explores_*` pattern is the right runner: it can spawn
windows in varying orders, sample the bar, and report mismatches.
