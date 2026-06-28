# 20 — idle / DPMS live apply (v26): display power + ext-idle-notify

**What**: validate the v26 idle/DPMS path on a live qdwin DRM session — the
`set_display_power` request (DPMS all outputs off/on) and the capability gate
the qdshell Power tab's inactivity-action + display-off timers ride on (driven
by the standard `ext-idle-notify-v1`).

**Why**: v26 flips the Power tab's idle policy from persist-only to live
(`CapabilityService.idleDpms`). The shell owns the idle *timing*
(ext-idle-notify) and the inactivity *action* (suspend/lock — a session
decision); the compositor only enacts display power via `set_display_power`.
`CapabilityService.idleDpms` is true only when BOTH halves are present: a >= v26
shell bind (`set_display_power`) AND the ext-idle-notify client (an
`ext_idle_notifier_v1` + a `wl_seat` bound by the qml-plugin).

## Environment

Standard qdwin GUI harness (`tests/gui/AGENTS.md`): a running libvirt domain on
`qemu:///session` with `qdwin-compositor.service` (weston + qdwin-shell.so) and
`qdshell.service` (qdshell). **The session is already fully provisioned by the
GUI gate** — the vendored libweston, qdwin-shell.so, qdshell, and the qml-plugin
are baked into the VM image (built fresh from the host source tree for this run)
and the user units are active before the scenario runs. Do NOT build or deploy
anything in-VM; just probe the live session. (If a precondition probe fails,
that is an ERROR to report, not a cue to provision.)

> Note on service names: the legacy `noctalia-session.service` /
> `noctalia-shell.service` units were retired (2026-06-16). The deployed
> contract is `qdwin-compositor.service` + `qdshell.service` +
> `qdwin-session.target`, and capabilities are read via the qdshell IPC
> (`qs ipc call qdwin capabilities`), NOT by grepping journald for unit logs.

## Path A — capability gate (qdshell IPC, deterministic)

Read the idle/DPMS capability through the stable qdshell IPC contract, gated on
a fully-bound v26+ session.

```bash
source ${QDWIN_REPO}/tests/gui/qdwin-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_session_healthy || { echo "ERROR: qdwin/qdshell user session not up"; exit 1; }

# qs_ipc <method> [args...] — call a qdwin IPC method on the running qdshell
# instance. Same proven-working invocation as 16/17/19 (`runuser -u admin --
# env … WAYLAND_DISPLAY=wayland-1 qs ipc -p PATH call qdwin …`), with a PID
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

# Readiness gate: poll until the shell reports a fully-bound v26+ session.
CAPS=
for _ in $(seq 1 30); do
    CAPS=$(qs_ipc capabilities)
    ver=$(printf '%s' "$CAPS" | sed -nE 's/.*version=([0-9]+).*/\1/p')
    case "$CAPS" in
        *bound=true*) [ -n "$ver" ] && [ "$ver" -ge 26 ] && break ;;
    esac
    sleep 1
done
echo "capabilities: $CAPS"
```

**Assert (A.0):** `$CAPS` contains `bound=true` and `version=` >= 26 (the
deployed build binds at v28). If `bound=true` never appears, the qdshell↔qdwin
binding is unreachable — record ERROR (precondition), not a product FAIL.

**Assert (A.1 — idle/DPMS capability, robust read):** the idle/DPMS capability
is live. Read it deterministically, preferring the IPC field and falling back
to the journal transition line:

```bash
# Preferred: the v26 IPC `idleDpms=` field (added to qdwin capabilities()).
# Present whenever the image was built from a qdshell that carries the field
# (the GUI golden is built fresh from host source for the run, so it normally
# is). If the field is absent (an older baked image predating it), fall back
# to the journal transition line emitted by CapabilityService on bind.
if printf '%s' "$CAPS" | grep -q 'idleDpms='; then
    printf '%s' "$CAPS" | grep -q 'idleDpms=true' \
        && echo "A.1 PASS (IPC idleDpms=true)" \
        || { echo "FAIL: idleDpms=false in IPC capabilities ($CAPS)"; exit 1; }
else
    # Fallback: restart qdshell for a clean bind, then poll the user journal
    # for the `idleDpms -> true` transition (module tag is the 14-char-padded
    # `CapabilityServ`). This line is emitted from Qdwin.qml's onBoundChanged /
    # onIdleNotifierAvailableChanged once both halves are present.
    echo "IPC idleDpms field absent (older image) — falling back to journal transition"
    CUR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
      --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")
    "$QDWIN_VM_EXEC" "$VMNAME" \
        "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
         systemctl --user restart qdshell.service"
    ok=
    for _ in $(seq 1 40); do
        "$QDWIN_VM_EXEC" "$VMNAME" \
          "journalctl _UID=1000 --after-cursor='$CUR' --no-pager 2>/dev/null" \
          | grep -q 'idleDpms -> true' && { ok=1; break; }
        sleep 1
    done
    [ -n "$ok" ] && echo "A.1 PASS (journal idleDpms -> true)" \
        || { echo "FAIL: no idleDpms=true (IPC field absent and no journal transition)"; exit 1; }
fi
```

This single read proves all three requirements held in the real session: a
>= v26 shell bind (`set_display_power`), `ext_idle_notifier_v1` bound, and a
`wl_seat` bound — i.e. the qml-plugin's ext-idle-notify client connected.

## Path B — compositor functional proof (bystander as shell)

Drive the compositor's `set_display_power` directly on the live DRM session,
independent of qdshell's init, using `qdwin-bystander` over its FIFO. Uses the
shared, self-healing take-over helpers in `tests/apps/qdwin-apps-helpers.sh`
(do NOT hand-roll the `systemctl stop` + bystander launch; the canonical helper
sets `XDG_RUNTIME_DIR=/run/user/1000`, `WAYLAND_DISPLAY` to the live socket, and
the FIFO at `/run/user/1000/qdwin-cmd.fifo`).

```bash
source ${QDWIN_REPO}/tests/apps/qdwin-apps-helpers.sh
# Resolve VMNAME independently so Path B is runnable on its own (e.g. an agent
# debugging just the bystander proof), not only after Path A set it.
VMNAME="${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_apps_set_vm "$VMNAME"

# PRECONDITION (infra): the bystander driver must be installed. Absence is an
# ERROR (cannot exercise the scenario), NOT a product FAIL. (The driver IS
# present on the baked image; a "no binary" reading is usually a stray
# `systemctl stop` erroring first because XDG_RUNTIME_DIR was unset — the
# helper exports it.)
"$QDWIN_VM_EXEC" "$VMNAME" 'command -v qdwin-bystander >/dev/null' \
    || { echo "ERROR: qdwin-bystander not installed on VM (cannot drive DPMS)"; exit 1; }

# Take over the shell role with the bystander; restore qdshell on ANY exit.
# Arm the restore trap IMMEDIATELY after a successful takeover (before the
# session-health check) so a later failure never leaves the desktop headless.
qdwin_apps_become_shell || { echo "ERROR: could not take over shell role"; exit 1; }
trap 'qdwin_apps_restore_shell' EXIT
qdwin_apps_session_up   || { echo "ERROR: bystander session not healthy"; exit 1; }

CURSOR=$("$QDWIN_VM_EXEC" "$VMNAME" "journalctl _UID=1000 -n 1 \
  --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'")

qdwin_apps_ctl displaypower 0    # DPMS all outputs off
sleep 0.5
qdwin_apps_ctl displaypower 1    # DPMS all outputs on
sleep 1

LOGS=$("$QDWIN_VM_EXEC" "$VMNAME" \
  "journalctl _UID=1000 --after-cursor='$CURSOR' --no-pager 2>/dev/null")
printf '%s\n' "$LOGS" | grep -E 'qdwin: set_display_power'
```

**Assert (B.1):** `qdwin: set_display_power on=0 (N output…)` then
`on=1 (N output…)` appear in the compositor journal since `$CURSOR` — the real
(virtual) output was power-cycled.
**Assert (B.2):** no `qdwin_shell_v1: … protocol error` between `$CURSOR` and
now (the bystander bound the shell role cleanly at >= v26).

## Cleanup

```bash
qdwin_apps_restore_shell   # also fires on the EXIT trap; idempotent — restarts qdshell.service
```

## Pass criteria

Path A (A.0 + A.1) mandatory and deterministic — this is the v26 capability
contract. Path B (B.1 + B.2) mandatory — the compositor functional proof. Write
`status.txt` PASS once both hold and STOP.

## Known-broken-if

- A.1 IPC `idleDpms=false` while `bound=true version>=26`: one half of the gate
  is missing. Check `Services/Qdwin/Qdwin.qml`'s `_refreshIdleDpmsCapability`
  (needs `bound && shellVersion >= 26 && idleNotifierAvailable`) — likely the
  ext-idle-notify client failed to bind `ext_idle_notifier_v1` + a `wl_seat`.
  Confirm with `wayland-info | grep -E 'qdwin_shell_v1|ext_idle_notifier_v1|wl_seat'`.
- A.1 fell back to the journal but no `idleDpms -> true`: the bind happened
  before `ext_idle_notifier_v1` arrived and the re-evaluation on
  `onIdleNotifierAvailableChanged` didn't fire — a real qdshell/binding defect.
- B.1 silent: `set_display_power` never reached the compositor. Confirm the
  bystander bound at >= v26 (B.2) and the FIFO command was accepted
  (`qdwin-bystander: cmd displaypower on=0` in `/tmp/bystander.log`).
- The desktop is left headless after the run: the EXIT trap /
  `qdwin_apps_restore_shell` didn't restart `qdshell.service`. Restart it
  manually before reporting.

## Not covered here

The full idle *trigger* (wait N minutes → `idled` → action / DPMS-off) is
timing-bound; the executable smoke `agent-idle-dpms-recovery-smoke.sh` covers
it live (short `power.displayOff*` / `inactivityTimeout*`, watch the
`PowerService` "idle policy armed" + "display-off idle -> DPMS off" lines, then
move the pointer to fire `resumed` → `set_display_power(1)`). Presentation mode
(`IdleInhibitorService`) suppressing both is verified there too.
