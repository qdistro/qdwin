# 12 — single-window RDP sharing: subscribe + xfreerdp sees frames

**Acceptance criterion:** a v14+ shell client can `subscribe_view_stream`
on a live toplevel handle, qdwin spawns `qdistro-forward`, the
forward listens on the announced TCP port, and a real RDP client
(xfreerdp) completing TLS + RDP handshake against that port can
receive at least one frame from the bound toplevel.

This covers the deepest end-to-end §6.5 path that the existing
`compositor-shell.bats::s3c-e2e` test only partially exercises
(it asserts the port accepts + frames flow inside qdistro-forward,
not that a real RDP client successfully decodes them).

## Prerequisites

This scenario REQUIRES:
- `freerdp3` (or `freerdp2`) installed in the VM (`xfreerdp` or
  `xfreerdp3` on $PATH). The disposable VMs use QEMU user networking, so
  their guest-only RDP listener has no host-routable address.
- `qdistro-forward` installed on the VM (built via
  `qdistro/daemons/forward/` — present in the baseweed bake).
- `qdwin-bystander` v ≥ 2026-05-14 installed on the VM (its
  `--subscribe HANDLE` flag is what drives the request — see
  `qdwin/test-client/qdwin-bystander.c`).
- `[pipewire] num-outputs >= 1` in the VM's weston.ini, OR the
  §6.5 spike bake's pipewire-bake provisioning. Without a free
  pipewire output, qdwin emits `denied "no free pipewire output"`
  and the rest of the scenario short-circuits.

Fail loudly if any of the *infrastructure* prereqs above (VM `xfreerdp`,
VM `qdistro-forward`, `qdwin-bystander`, a free pipewire output) is missing;
do not skip those. The *subject* app `foot`, however, is part of the opt-in
`QDWIN_APP_DEPS` matrix: on a lean GUI golden (no `QDWIN_APP_DEPS=1`) it is
legitimately absent, so SKIP cleanly per the apps/AGENTS.md rule rather than
ERROR (see the Setup guard below).

## Setup

```bash
source ${QDWIN_REPO}/tests/apps/qdwin-apps-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_session_healthy || { echo "FAIL: qdwin session not up"; exit 1; }

# VM: confirm xfreerdp exists. The RDP listener is guest-local under QEMU user
# networking, so the real client must run in this same disposable VM.
"$QDWIN_VM_EXEC" "$VMNAME" 'command -v xfreerdp >/dev/null 2>&1 || command -v xfreerdp3 >/dev/null 2>&1' \
    || { echo "FAIL: install freerdp3 in the VM"; exit 1; }

# VM: confirm qdistro-forward.
"$QDWIN_VM_EXEC" "$VMNAME" 'test -x /usr/bin/qdistro-forward' \
    || { echo "FAIL: qdistro-forward not installed on VM"; exit 1; }

# Subject app: foot is the toplevel shared over RDP and is part of the opt-in
# qdwin app-deps matrix. On a lean GUI golden (no QDWIN_APP_DEPS=1) it is
# legitimately absent — SKIP cleanly rather than ERROR, matching apps/05/07/08.
if ! "$QDWIN_VM_EXEC" "$VMNAME" 'command -v foot >/dev/null 2>&1'; then
    echo "SKIP: foot not installed; qdwin app deps are opt-in (rerun with QDWIN_APP_DEPS=1)"
    exit 0
fi

"$QDWIN_VM_EXEC" "$VMNAME" 'pkill -u admin -x foot 2>/dev/null; sleep 1' >/dev/null

# Spawn a foot terminal to share.
"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 foot sleep 600 >/dev/null 2>&1 &' " >/dev/null
sleep 2

HANDLE=$("$QDWIN_VM_EXEC" "$VMNAME" \
    "journalctl _UID=1000 --no-pager | grep 'qdwin: toplevel_added' \
     | tail -1 | sed -nE 's/.*handle=([0-9]+).*/\1/p'")
[ -n "$HANDLE" ] || { echo "FAIL: no toplevel handle"; exit 1; }
echo "subject toplevel handle=$HANDLE"
```

## Steps

### Step 1 — subscribe + capture the approved event

The shell-side request:

```c
qdwin_shell_v1_subscribe_view_stream(shell, HANDLE, "...", 0, 0, 0);
```

emits the `approved` event with `(pipewire_node_name, rdp_port,
rdp_cert_path, rdp_password)`. Drive this from the VM via
`qdwin-bystander --subscribe`, which prints sh-sourceable KEY=value
lines on stdout when `approved` fires and keeps the wayland
connection open so the stream stays live:

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
    "runuser -l admin -c 'XDG_RUNTIME_DIR=/run/user/1000 \
     WAYLAND_DISPLAY=wayland-1 nohup /usr/bin/qdwin-bystander \
       --subscribe $HANDLE \
       > /tmp/15-creds.env 2>/tmp/15-bystander.err & echo \$!'" \
    > /tmp/15-bystander.pid
sleep 1
. <(printf '\n'; "$QDWIN_VM_EXEC" "$VMNAME" 'cat /tmp/15-creds.env')

# Variables now in scope: HANDLE, PIPEWIRE_NODE_NAME, RDP_PORT,
# RDP_CERT_PATH, RDP_PASSWORD. FORWARD_PID is not exposed via the
# protocol — derive from journal if needed.
echo "rdp_port=$RDP_PORT node=$PIPEWIRE_NODE_NAME"
```

If qdwin-bystander is absent on the VM (older bake), fail with
"deploy qdwin-bystander v >= 2026-05-14"; do not skip.

**Assert (1.1):** the journal shows
`qdwin: view_stream approved handle=$HANDLE peer_label=... pw=...
output_pos=... rdp_port=<P> forward_pid=<P> ...` within 2s of the
subscribe request. (If instead the journal shows
`qdwin: subscribe_view_stream denied handle=$HANDLE ... (no pw
output)`, weston.ini lacks `[pipewire] num-outputs>=1` — that's a
prereq failure, fail loud.)

**Assert (1.2):** $RDP_PORT is a valid TCP port (1024..65535).

### Step 2 — VM-side TCP accept

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
    "timeout 3 bash -c 'echo > /dev/tcp/127.0.0.1/$RDP_PORT' && echo TCP_OPEN"
```

**Assert (2.1):** prints `TCP_OPEN` (qdistro-forward is listening
on the announced port and the kernel accepts a connection).

### Step 3 — guest-local full xfreerdp session

```bash
RDP_CLIENT_B64=$(base64 -w0 <<EOF
set -o pipefail
runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 DISPLAY=:0 \\
  timeout 8 xfreerdp /v:127.0.0.1:$RDP_PORT /cert:ignore \\
  /u:test /p:$RDP_PASSWORD /size:640x480 +decorations -encryption \\
  > /tmp/15-xfreerdp.log 2>&1
rc=\$?
# A full client remains connected until the deliberate timeout. Any earlier
# exit is a handshake/auth/framebuffer failure and must stay red.
[ "\$rc" -eq 124 ] || { cat /tmp/15-xfreerdp.log; exit "\$rc"; }
EOF
)
"$QDWIN_VM_EXEC" "$VMNAME" "echo $RDP_CLIENT_B64 | base64 -d | bash"
```

**Assert (3.1):** qdistro-forward's log shows `auth OK for user=test` and
xfreerdp's guest log shows both local and remote framebuffer initialization.
The client must remain connected until the deliberate timeout. This exercises
the shadow server's post-connect authentication path; `+auth-only` exits before
that path in FreeRDP 3.30 and is not a valid authentication test.

Failure modes that should fail this assert:

- Exit before the deliberate timeout → handshake, auth, or framebuffer setup failed.
- "TLS connection failed" → cert/port mismatch.
- "Failed to authenticate" → password mismatch (rotation race?).

### Step 5 — disconnect cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
    "kill $FORWARD_PID 2>/dev/null; sleep 1; \
     ps -p $FORWARD_PID 2>&1 | tail -1"
```

**Assert (5.1):** qdistro-forward exits cleanly when killed.
qdwin should emit `qdwin: view_stream_torn_down handle=$HANDLE
pid=$FORWARD_PID` shortly after.

## Cleanup

```bash
"$QDWIN_VM_EXEC" "$VMNAME" \
    'kill $(pgrep -f qdistro-forward) 2>/dev/null; \
     pkill -u admin -x foot 2>/dev/null; true' >/dev/null
```

## Pass criteria

Asserts 1.1, 1.2, 2.1, 3.1, and 5.1 pass. The original `s3c-e2e` test in
`compositor-shell.bats` proves the qdistro-forward → port-accepts
+ frames-flow path; THIS scenario adds the missing real-RDP-client
half (xfreerdp completing the TLS handshake and decoding frames).

## Known-broken-if

- 1.1 silent: subscribe path in qdwin's
  `qdwin_handle_subscribe_view_stream` is reaching the send but
  not logging — adjacent gap to the 2026-05-14 keybinding
  instrumentation fix.
- 2.1 connection refused: qdistro-forward never spawned, OR
  spawned and crashed. Check `journalctl -u user@1000` for
  forward.c crash signatures.
- 3.1 TLS failure: `rdp_cert_path` points at a non-existent or
  expired cert. Check `QDWIN_RDP_CERT` env on the compositor
  systemd unit; default is `/etc/qdwin/rdp-cert.pem` per
  forward.c.
- 3.1 auth failure: password from subscribe doesn't match.
  Rotation race — qdistro-forward may have generated a new
  password between subscribe and xfreerdp launch. Re-subscribe.
- 3.1 framebuffer failure: frames flow in qdistro-forward (s3c-e2e
  PASSes) but xfreerdp cannot initialize its framebuffers. Inspect the
  guest `/tmp/15-xfreerdp.log` with `wlog.level=debug`.

## History

Originally (pre-2026-05-14) this scenario could only run against
§6.5-baked VMs because it depended on
`/root/s3c-subscribe-extract.sh` to drive the request. As of
2026-05-14, `qdwin-bystander --subscribe <handle>` carries the
subscribe wire request and prints sh-sourceable credentials on
stdout when `approved` fires, so the scenario runs against any VM
that has `qdwin-bystander` + `qdistro-forward` installed and a
free pipewire output. The deterministic regression
`t_bystander_subscribe_sends_request` in
`qdistro/scripts/install/gui-regression-tests.sh` exercises the
wire path on the deny branch (the more common state on non-spike
VMs); the visual half of this scenario covers the approved branch
plus the real xfreerdp handshake.
