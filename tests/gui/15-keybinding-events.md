# 15 — qdwin keybinding events emit journal log lines

**Acceptance criterion:** every keybinding handled by the compositor
(Ctrl+Space launcher, Alt+Tab switcher, Ctrl+Alt+L lock, registered
hotkeys, overlay-grab keys) emits a `qdwin: <event>` log line
**independent of qdshell binding state**. Without this, the
keybinding code paths silently drop input on a missing/buggy shell
and the silent-drop bug class (the suite's motivating problem)
escapes detection. See
`todo/qdwin-keybindings-uninstrumented.md` for the diagnosis and
the 2026-05-14 fix.

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

"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x foot 2>/dev/null; sleep 1' >/dev/null
```

## Steps

### Step 1 — Ctrl+Space drives launcher_requested

```bash
CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
qdwin_chord ctrl -- spc
sleep 0.5
qdwin_send_key KEY_ESC                          # dismiss launcher
sleep 0.3
"$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --after-cursor='$CURSOR' --no-pager | \
   grep -E 'qdwin: launcher_requested'"
```

**Assert (1.1):** at least one `qdwin: launcher_requested` log line.

**Assert (1.2):** if qdshell is running and bound at v>=1 the launcher
overlay is briefly visible — capture a screenshot mid-chord
(`qdwin_screenshot /tmp/15-step1-launcher.png`) and check for the
launcher's distinctive surface (`grep launcher` in journal works as
a proxy if the screenshot is ambiguous).

### Step 2 — Alt+Tab drives switcher_next + switcher_commit

```bash
# Need two windows for the switcher to do meaningful work.
for i in 1 2; do
    "$QDWIN_VM_EXEC" "$VMNAME" \
      "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
       WAYLAND_DISPLAY=wayland-1 foot sleep 600 &' " >/dev/null
    sleep 1
done
CURSOR2=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
qdwin_chord alt -- tab
sleep 0.5
"$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --after-cursor='$CURSOR2' --no-pager | \
   grep -E 'qdwin: switcher_(next|commit)'"
```

**Assert (2.1):** at least one `qdwin: switcher_next dir=1` line
(forward direction) AND at least one `qdwin: switcher_commit cause=…`
line on Alt-release. The `cause=` tag distinguishes the
mod-fallback / alt-released / non-tab-key code paths.

### Step 3 — Ctrl+Alt+L drives lock_requested or bound-shell warning

```bash
CURSOR3=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
qdwin_chord ctrl alt -- l
sleep 0.6
"$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --after-cursor='$CURSOR3' --no-pager | \
   grep -E 'qdwin: lock_requested|qdwin: lock key pressed'"
```

**Assert (3.1):** exactly one of:

- `qdwin: lock_requested` (shell bound at v>=7, normal path).
- `qdwin: lock key pressed; no shell bound` (no shell, log-only).
- `qdwin: lock key pressed but shell bound <v7` (old-version shell).

The exact line indicates which branch fired; ALL three are valid
log lines and ALL prove the keybinding was processed by the
compositor (the canonical silent-drop motivating bug would emit
none of them).

### Step 4 — Hotkey registered via shell fires hotkey_pressed

A registered hotkey (via `qdwin_shell_v1.register_hotkey`) should
emit `qdwin: hotkey_pressed id=N` on each press. This requires a
shell-bound client to register the hotkey first; the
qdwin-bystander test client can do this when run with
`--register-hotkey <id> <mods> <key>`. Skip this step if
qdwin-bystander isn't built or the shell isn't a test client.

```bash
# (Optional — only if running with qdwin-bystander as shell.)
# qdwin_send_key KEY_PRINT
# journalctl ... | grep "qdwin: hotkey_pressed id="
```

**Assert (4.1):** `qdwin: hotkey_pressed id=<expected>` on the
keystroke. **Skipped when no registering shell is present.**

## Cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x foot 2>/dev/null; true' >/dev/null
```

## Pass criteria

Asserts 1.1, 2.1, 3.1 pass; 4.1 conditional on test-shell presence.
Confirms the 2026-05-14 keybinding-instrumentation fix
(`qdwin/qdwin.c`) is wired correctly across all four code paths.

## Known-broken-if

- 1.1 silent: `qdwin_handle_launcher_key` (or the
  `qdwin_shell_v1_send_launcher_requested` site) is missing the
  preceding `weston_log` call.
- 2.1 missing `switcher_next` but `switcher_commit` present: the
  switcher_grab tab-handling branch is reaching the send but
  bypassing the log (look at the `if (key == KEY_TAB)` block in
  `qdwin_switcher_grab_key`).
- 2.1 missing both: weston modifier_binding never fired — confirm
  you used `qdwin_chord alt -- tab` (QMP), NOT
  `qdwin_send_key KEY_LEFTALT KEY_TAB` (virsh send-key). See the
  AGENTS.md "Why two key paths" post-mortem.
- 3.1 silent: `qdwin_handle_lock_key` reached but didn't reach any
  log branch — gate inversion somewhere.
