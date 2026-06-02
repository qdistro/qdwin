# 07 — qdwin_locker_v1 bind gate (uid gate + one-locker-at-a-time)

**What**: drive `qdwin-locker-probe` against the headless compositor to
check qdwin's `qdwin_locker_v1` bind gate (`bind_qdwin_locker` +
`qdwin_handle_bind_as_locker`, `qdwin/qdwin.c`): an unauthorized uid is
refused at global bind, an authorized client binds and gets `ready`, and
the "only one locker may exist at a time" semantics in the XML hold
(double `bind_as_locker` on one resource is rejected with `already_bound`;
a second locker bind while the first peer is alive is REFUSED with
`locker_present` — the live locker is NOT evicted). It also checks the
production default makes the locker exe mandatory (FINDING #5).

**Why**: the locker is the only client allowed to drive compositor `locked`
state. If an arbitrary same-session client could bind the locker global, it
could black-screen the session or, worse, swap the lock UI. The gate keys
on the peer uid (`uid != allowed_locker_uid` → implementation error) with
optional defence-in-depth exe/SELinux checks (df8f3d5). This scenario is
the headless half of the locker bind-gate item in
`todo/codex-testing/under-tested-areas.md` §3.

**Non-visual**: asserts on probe exit codes and weston-log evidence, not
screenshots — fully scriptable.

**Headless / scope notes (read before relying on this)**:
- `allowed_locker_uid` has no separate CLI/env knob today: qdwin defaults
  it to `(uid_t)-1` and falls back to `allowed_uid` at init
  (`qdwin.c`, search `allowed_locker_uid == (uid_t)-1`). So a foreign
  `QDWIN_ALLOWED_UID` (via `start.sh --allowed-uid`) is exactly what makes
  the locker uid gate reject — no `--allowed-locker-uid` was needed.
- A single-uid host cannot run a client under a second real uid, so the
  "unauthorized" case is driven with a foreign `allowed_uid` + `--no-shell`
  (the probe's real uid then mismatches `allowed_locker_uid`). This
  faithfully exercises the uid reject branch.
- The locker policy now defaults to a MANDATORY exe (`/usr/bin/qdlocker`)
  when no explicit exe/label is configured (FINDING #5(a) fail-closed). The
  headless probe is not that exe, so `start.sh` exports
  `QDWIN_ALLOWED_LOCKER_ANY=1` to drop to the legacy uid-only policy for the
  uid-gate positive paths (S2–S4). S5 overrides that to verify the
  production mandatory-exe default itself. The optional
  `--qdwin-allowed-locker-exe=` / `--qdwin-allowed-locker-label=` MATCH paths
  (a peer whose exe/label matches) still need a VM/policy follow-up.
- Overlay-key routing while locked and lock-surface destroy/recreate need a
  real lock cycle + input injection — left as VM follow-ups (see
  `tests/gui/`), not attempted headless.

## Setup

```bash
HT=tests/host
. "$HT/lib.sh"
PROBE="$QDWIN_INSTALL/bin/qdwin-locker-probe"
FOREIGN_UID=$(( $(id -u) + 1 ))   # any uid != this one
```

## Steps

### S1 — unauthorized uid is REFUSED at bind

Start qdwin with a foreign allowed_uid and no shell, so the probe's real
uid matches neither `allowed_locker_uid` nor the shell.

```bash
ID=07-locker-reject
$HT/start.sh $ID --no-shell --no-terminal --allowed-uid "$FOREIGN_UID" >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID); WLOG=$(ht_log_weston $ID)

# lib.sh runs under `set -e` and S1 EXPECTS a nonzero probe exit, so
# capture it set-e-safe (a bare `cmd; RC=$?` would abort the script). The
# probe exits 4 ONLY when the bind was refused with the expected
# implementation error on wl_display (a wrong/unexpected error exits 1), so
# RC==4 already rules out an unrelated disconnect.
if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE"; then RC=0; else RC=$?; fi
# HONEST LOG NOTE: the uid reject is posted to the CLIENT as an
# implementation error ("uid N not permitted") — unlike secctx (06) the
# locker has NO dedicated "REJECTED" weston-log line. The only server-log
# evidence is the bind-attempt line showing the uid mismatch, plus the
# ABSENCE of a "locker bound" line (no binding was created). Assert both:
# a mismatched bind-attempt line is present AND no "locker bound" appeared.
if grep -qE "locker bind attempt .* uid=$(id -u) .*allowed_locker_uid=$FOREIGN_UID" "$WLOG"; then ATTEMPT=0; else ATTEMPT=1; fi
if grep -qF "qdwin: locker bound" "$WLOG"; then BOUND=1; else BOUND=0; fi
$HT/stop.sh $ID
if [ "$RC" -eq 4 ] && [ "$ATTEMPT" -eq 0 ] && [ "$BOUND" -eq 0 ]; then echo "S1 PASS"; else
    echo "S1 FAIL (exit=$RC want 4; attempt-log=$ATTEMPT want 0; bound-log=$BOUND want 0)"; fi
```

**Assert (S1):** all must hold (the conjunction is the pass — a bind error
alone could be an unrelated disconnect):
- `RC == 4` — the probe's bind round-trip saw the EXPECTED implementation
  error on wl_display (gate refused the bind). A wrong/unexpected error
  exits 1 and fails. PRIMARY client-observed signal.
- the weston log contains a `locker bind attempt … uid=<this> …
  allowed_locker_uid=<foreign>` line — proves the request reached the uid
  gate with a real mismatch. HARD.
- the weston log contains NO `qdwin: locker bound` line — proves no
  binding was created. HARD. (The "uid N not permitted" text is posted to
  the client, not the server log, so it is observed via `RC` instead.)

### S2 — authorized client is ACCEPTED (gets `ready`)

Start qdwin normally (allowed_uid == this uid). The probe binds and
`bind_as_locker` must succeed (the `ready` event fires).

```bash
ID=07-locker-allow
$HT/start.sh $ID --no-shell --no-terminal >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID); WLOG=$(ht_log_weston $ID)

if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE"; then RC=0; else RC=$?; fi
if grep -qF "qdwin: locker bound" "$WLOG"; then LOGHIT=0; else LOGHIT=1; fi
$HT/stop.sh $ID
if [ "$RC" -eq 0 ] && [ "$LOGHIT" -eq 0 ]; then echo "S2 PASS"; else
    echo "S2 FAIL (exit=$RC want 0; bound-log hit=$LOGHIT want 0)"; fi
```

**Assert (S2):**
- `RC == 0` — bind accepted AND `bind_as_locker` round-tripped with the
  `ready` event delivered (the probe checks for it explicitly).
- the weston log contains `qdwin: locker bound pid=… uid=…` (HARD).

### S3 — second `bind_as_locker` on the same resource is REJECTED (`already_bound`)

The locker XML: "qdwin rejects a second bind from the same client". The
probe binds once, then calls `bind_as_locker` twice on the same resource;
the second must raise the dedicated `already_bound` (=1) error.

```bash
ID=07-locker-double
$HT/start.sh $ID --no-shell --no-terminal >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID)

# exit 3 = the expected already_bound rejection (probe's PASS signal); any
# other value (incl. a generic exit 1 from a *different* error) is a fail.
if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE" --double-bind; then RC=0; else RC=$?; fi
$HT/stop.sh $ID
if [ "$RC" -eq 3 ]; then echo "S3 PASS"; else
    echo "S3 FAIL (exit=$RC want 3 = already_bound)"; fi
```

**Assert (S3):**
- `RC == 3` — the probe saw the FIRST `bind_as_locker` succeed (`ready`)
  and the SECOND raise exactly `QDWIN_LOCKER_V1_ERROR_ALREADY_BOUND` (=1).
  The probe distinguishes this from any other protocol error.

### S4 — a second locker bind is REFUSED while the first peer is ALIVE (FINDING #5)

Security fix: qdwin must NOT evict a live locker just because another
same-uid client binds. The probe binds the global twice (two resources on
one client — same live pid) and calls `bind_as_locker` on each. The first
becomes the live locker; because the probe process (the locker peer) is
still alive, the second `bind_as_locker` must be REFUSED with the
dedicated `locker_present` (=5) error and the original binding left
intact. Previously qdwin destroyed the old binding here, letting any
same-uid client take over the locker and call `set_locked(0)`.

```bash
ID=07-locker-rebind
$HT/start.sh $ID --no-shell --no-terminal >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID); WLOG=$(ht_log_weston $ID)

# exit 5 = the expected locker_present rejection (probe's PASS signal); any
# other value (incl. exit 1 from acceptance/eviction) is a fail.
if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE" --rebind; then RC=0; else RC=$?; fi
# Exactly ONE "locker bound" line (the first); the second was refused, so
# no replacement bound line is emitted. The refusal log line is present.
BOUND=$(grep -cF "qdwin: locker bound" "$WLOG" || true)
if grep -qF "qdwin: locker bind REFUSED" "$WLOG"; then REFUSED=0; else REFUSED=1; fi
$HT/stop.sh $ID
if [ "$RC" -eq 5 ] && [ "$BOUND" -eq 1 ] && [ "$REFUSED" -eq 0 ]; then echo "S4 PASS"; else
    echo "S4 FAIL (exit=$RC want 5; bound-count=$BOUND want 1; refused-log=$REFUSED want 0)"; fi
```

**Assert (S4):** all must hold:
- `RC == 5` — the second `bind_as_locker` raised exactly
  `QDWIN_LOCKER_V1_ERROR_LOCKER_PRESENT` (=5) on `qdwin_locker_v1`. The
  probe distinguishes this from any other protocol error and from
  acceptance. PRIMARY signal.
- exactly ONE `qdwin: locker bound` line — the live locker was NOT
  evicted/replaced. HARD.
- a `qdwin: locker bind REFUSED … takeover denied` line is present. HARD.

### S5 — mandatory-exe locker policy default (FINDING #5(a))

Security fix: with no explicit exe/label policy, qdwin must default to a
mandatory exe (`/usr/bin/qdlocker`) rather than uid-only. Start qdwin
WITHOUT the dev/test opt-out and confirm the startup log shows the exe
default (not `[UID-ONLY: INSECURE]`). The probe (not /usr/bin/qdlocker) is
then rejected at bind because its exe does not match.

```bash
ID=07-locker-defexe
QDWIN_ALLOWED_LOCKER_ANY= $HT/start.sh $ID --no-shell --no-terminal >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID); WLOG=$(ht_log_weston $ID)

# Policy line must name the default exe and must NOT be flagged uid-only.
if grep -qE "qdwin: locker bind policy .*exe=/usr/bin/qdlocker" "$WLOG"; then POL=0; else POL=1; fi
if grep -qF "[UID-ONLY: INSECURE]" "$WLOG"; then UIDONLY=1; else UIDONLY=0; fi
# The probe's exe != /usr/bin/qdlocker, so bind_as_locker is rejected; no
# "locker bound" line appears.
if XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE"; then RC=0; else RC=$?; fi
if grep -qF "qdwin: locker bound" "$WLOG"; then BOUND=1; else BOUND=0; fi
$HT/stop.sh $ID
if [ "$POL" -eq 0 ] && [ "$UIDONLY" -eq 0 ] && [ "$BOUND" -eq 0 ]; then echo "S5 PASS"; else
    echo "S5 FAIL (policy-exe=$POL want 0; uid-only-flag=$UIDONLY want 0; bound=$BOUND want 0; probe-rc=$RC)"; fi
```

**Assert (S5):**
- the weston log policy line reads `exe=/usr/bin/qdlocker` (mandatory exe
  defaulted) and NOT `[UID-ONLY: INSECURE]`. HARD.
- NO `qdwin: locker bound` line — the non-matching-exe probe was rejected,
  so uid-only takeover is closed. HARD.

> Note: `start.sh` exports `QDWIN_ALLOWED_LOCKER_ANY=1` by default so the
> uid-only positive paths (S2–S4) work with the test probe. S5 overrides
> it to empty to exercise the production mandatory-exe default.

## Teardown

`stop.sh` runs inline per case. If a case aborts before its `stop.sh`,
tear each one down individually:
`$HT/stop.sh 07-locker-reject`, `$HT/stop.sh 07-locker-allow`,
`$HT/stop.sh 07-locker-double`, `$HT/stop.sh 07-locker-rebind`,
`$HT/stop.sh 07-locker-defexe`.

## Pass criteria

All five asserts hold: S1 `RC==4` (implementation-error reject) +
bind-attempt mismatch log + NO bound log, S2 `RC==0` + bound log, S3
`RC==3` (already_bound), S4 `RC==5` (locker_present) + exactly 1 bound
line + REFUSED log, S5 mandatory-exe policy line + no uid-only flag + no
bound line.
