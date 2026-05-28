# 06 — secctx manager bind gate (Option-A launcher gate)

**What**: drive `qdwin-secctx-probe` against the headless compositor in
three configurations and check that qdwin's
`wp_security_context_manager_v1` bind gate (`bind_qdwin_secctx_manager`,
`qdwin/qdwin.c`) refuses an unauthorized client, accepts an authorized
one (and lets it commit a listener), and honours the `QDWIN_SECCTX_OPEN`
dev override.

**Why**: the broker-attested secctx identity contract
(`todo/decisions/secctx-identity-contract.md`, Option A) is only sound if
the compositor actually refuses manager binds from clients that are
neither the bound qdshell nor `allowed_uid`. Self-asserted secctx strings
from an arbitrary same-session client would otherwise let it impersonate
another silo. This is the headless half of the open checklist item
"assert an unauthorized uid is refused at manager bind while the trusted
launcher succeeds and can commit a listener".

**Non-visual**: this scenario asserts on probe exit codes and weston-log
branch evidence, not screenshots — it is fully scriptable, no vision step.

**Headless limitation (read before relying on this)**: the gate keys on
`(!from_shell && uid != allowed_uid)`. A single-uid headless host cannot
run a client under a *second real uid*, so the "unauthorized" case is
driven by starting qdwin with a **foreign `QDWIN_ALLOWED_UID`** (via
`start.sh --allowed-uid`) plus `--no-shell`, so neither the uid branch
nor the from_shell branch can admit the probe. The "authorized" case uses
the normal `allowed_uid` (== this uid). This faithfully exercises the
reject branch and the allow+commit path, but it does **not** isolate the
`from_shell`-despite-wrong-uid branch — proving "the shell client is
admitted even under a wrong uid" needs the shell under a different real
uid (root / the multi-uid VM suite, `tests/gui/03-multi-uid-colours`).
That sub-case is intentionally left to a VM follow-up.

## Setup

```bash
HT=tests/host
. "$HT/lib.sh"
PROBE="$QDWIN_INSTALL/bin/qdwin-secctx-probe"
FOREIGN_UID=$(( $(id -u) + 1 ))   # any uid != this one
```

## Steps

### S1 — unauthorized client is REFUSED

Start qdwin with a foreign allowed_uid and no shell, so the probe (real
uid = this uid, not the shell) matches neither admit condition.

```bash
ID=06-secctx-reject
$HT/start.sh $ID --no-shell --no-terminal --allowed-uid "$FOREIGN_UID" >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID); WLOG=$(ht_log_weston $ID)

# Capture nonzero exits explicitly: lib.sh runs under `set -e`, and S1
# EXPECTS the probe to fail (exit 1), so a bare `cmd; RC=$?` would abort.
if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE"; then RC=0; else RC=$?; fi
if grep -qF "secctx: REJECTED manager bind" "$WLOG"; then LOGHIT=0; else LOGHIT=1; fi
$HT/stop.sh $ID
if [ "$RC" -eq 1 ] && [ "$LOGHIT" -eq 0 ]; then echo "S1 PASS"; else
    echo "S1 FAIL (exit=$RC want 1; reject-log hit=$LOGHIT want 0)"; fi
```

**Assert (S1):** both must hold (the conjunction is the pass — a bind
error alone could be an unrelated disconnect):
- `RC == 1` — the probe's bind round-trip saw a protocol/implementation
  error (the gate refused the bind). PRIMARY client-observed signal.
- the weston log contains `qdwin/secctx: REJECTED manager bind from
  uid=…` — proves the refusal came from the intended branch. This is a
  HARD assertion, not advisory.

### S2 — authorized client is ACCEPTED and can commit a listener

Start qdwin normally (allowed_uid == this uid). The probe now matches the
uid branch and must bind AND complete a `create_listener` + `commit`.

```bash
ID=06-secctx-allow
$HT/start.sh $ID --no-shell --no-terminal >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID); WLOG=$(ht_log_weston $ID)

if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE" --commit; then RC=0; else RC=$?; fi
if grep -qF "secctx: manager bound" "$WLOG"; then LOGHIT=0; else LOGHIT=1; fi
$HT/stop.sh $ID
if [ "$RC" -eq 0 ] && [ "$LOGHIT" -eq 0 ]; then echo "S2 PASS"; else
    echo "S2 FAIL (exit=$RC want 0; bound-log hit=$LOGHIT want 0)"; fi
```

**Assert (S2):**
- `RC == 0` — bind accepted, and `create_listener` + the secctx setters +
  `commit` round-tripped with no protocol error. This is the "can commit
  a listener" half of the open item: it proves the authorized client may
  create and commit a security-context listener. It does NOT exercise a
  client actually connecting *through* the committed listen socket —
  that is out of scope here.
- the weston log contains `qdwin/secctx: manager bound by uid=…`
  (HARD assertion).

> Note: with a normal allowed_uid the probe's uid equals allowed_uid, so
> this proves "an authorized client is admitted and can commit". It does
> NOT isolate uid-allow from shell-allow (see the headless-limitation
> note above).

### S3 — `QDWIN_SECCTX_OPEN=1` dev override admits the otherwise-rejected client

The same foreign-allowed_uid / no-shell setup as S1, but with the
documented developer escape hatch set, must now ACCEPT the bind.

```bash
ID=06-secctx-open
QDWIN_SECCTX_OPEN=1 $HT/start.sh $ID --no-shell --no-terminal \
    --allowed-uid "$FOREIGN_UID" >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID)

if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE"; then RC=0; else RC=$?; fi
$HT/stop.sh $ID
if [ "$RC" -eq 0 ]; then echo "S3 PASS"; else echo "S3 FAIL (exit=$RC want 0)"; fi
```

**Assert (S3):**
- `RC == 0` — with `QDWIN_SECCTX_OPEN=1` the gate is bypassed and the
  same client that was refused in S1 now binds successfully. (Confirms
  the override works AND that S1's refusal was the gate, not an unrelated
  failure.)

## Teardown

`stop.sh` runs inline per case. If a case aborts before its `stop.sh`,
tear each one down individually (one id per call):
`$HT/stop.sh 06-secctx-reject`, `$HT/stop.sh 06-secctx-allow`,
`$HT/stop.sh 06-secctx-open`.

## Pass criteria

All three asserts hold: S1 `RC==1` + reject log, S2 `RC==0` + bound log,
S3 `RC==0`.
