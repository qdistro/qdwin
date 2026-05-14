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
- `freerdp3` (or `freerdp2`) installed on the host (`xfreerdp` or
  `xfreerdp3` on $PATH).
- `qdistro-forward` installed on the VM (built via
  `qdistro/daemons/forward/` — present in the baseweed bake).
- `qdwin-bystander` v ≥ 2026-05-14 installed on the VM (its
  `--subscribe HANDLE` flag is what drives the request — see
  `qdwin/test-client/qdwin-bystander.c`).
- `[pipewire] num-outputs >= 1` in the VM's weston.ini, OR the
  §6.5 spike bake's pipewire-bake provisioning. Without a free
  pipewire output, qdwin emits `denied "no free pipewire output"`
  and the rest of the scenario short-circuits.

Fail loudly if any prereq is missing; do not skip.

## Setup

```bash
source ${QDWIN_REPO}/tests/apps/qdwin-apps-helpers.sh
qdwin_set_vm "${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -1)}"
qdwin_session_healthy || { echo "FAIL: qdwin session not up"; exit 1; }

# Host: confirm xfreerdp exists.
command -v xfreerdp >/dev/null 2>&1 || command -v xfreerdp3 >/dev/null 2>&1 \
    || { echo "FAIL: install freerdp3 on host"; exit 1; }

# VM: confirm qdistro-forward.
"$QDWIN_VM_EXEC" "$VMNAME" 'test -x /usr/bin/qdistro-forward' \
    || { echo "FAIL: qdistro-forward not installed on VM"; exit 1; }

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

### Step 3 — host xfreerdp handshake

```bash
VM_IP=$(virsh -c qemu:///session domifaddr "$VMNAME" \
        | awk '/ipv4/{print $4}' | sed 's|/.*||')
[ -n "$VM_IP" ] || { echo "FAIL: no VM IP"; exit 1; }

# +auth-only: TLS + auth + capability exchange only, NO video render.
# /cert:ignore: skip server-cert pinning (TOFU is a separate test).
timeout 15 xfreerdp /v:$VM_IP:$RDP_PORT \
    /cert:ignore \
    /u:test /p:$RDP_PASSWORD \
    +auth-only 2>&1 | tee /tmp/15-xfreerdp.log
```

**Assert (3.1):** xfreerdp's last log line is
`Authentication only, exit status` (or a successful disconnect)
and exit code is 0. Failure modes that should fail this assert:

- Exit code != 0 → handshake failed.
- "TLS connection failed" → cert/port mismatch.
- "Failed to authenticate" → password mismatch (rotation race?).

### Step 4 — frame capture path (visual)

For a full frame-level test, run xfreerdp WITHOUT `+auth-only`:

```bash
DISPLAY=:0 xfreerdp /v:$VM_IP:$RDP_PORT \
    /cert:ignore /u:test /p:$RDP_PASSWORD \
    /size:640x480 +decorations -encryption \
    /timeout:8000 &
XFREERDP_PID=$!
sleep 4
# Screenshot the xfreerdp output window
import -window xfreerdp /tmp/15-rdp-client.png 2>/dev/null \
  || gnome-screenshot -w -f /tmp/15-rdp-client.png 2>/dev/null \
  || true
kill $XFREERDP_PID 2>/dev/null; wait 2>/dev/null
ls -la /tmp/15-rdp-client.png
```

**Assert (4.1):** `/tmp/15-rdp-client.png` exists and is non-zero
size; visual inspection shows the foot terminal contents (the
remote view of the subject toplevel). This step requires a host
display server — skip if $DISPLAY isn't set and report
"requires X" rather than passing falsely.

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

Asserts 1.1, 1.2, 2.1, 3.1 pass at minimum (the
handshake-completing-end-to-end path). 4.1 + 5.1 pass when a host
display is available. The original `s3c-e2e` test in
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
- 4.1 black screen: frames flowing in qdistro-forward (s3c-e2e
  PASSes) but xfreerdp displays black. Pixel format mismatch
  between PipeWire output (default BGRA) and RDP wire format
  (typically RGB/16). Inspect the FreeRDP debug output with
  `wlog.level=debug`.

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
