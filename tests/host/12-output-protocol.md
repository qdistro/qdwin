# 12 — wlr-output-management-v1 server: enumerate + atomic apply/revert

**What**: drive `qdwin-output-probe` against the headless compositor to check
qdwin's `zwlr_output_manager_v1` server (the output/display-management
implementation, `qdwin/qdwin.c`): a freshly-bound manager streams one
`zwlr_output_head_v1` per output (with its modes, current mode, position,
scale, transform) terminated by a `done` carrying the configuration serial;
a configuration built against that serial can be `test`ed and `apply`ed
atomically (the compositor answers `succeeded`); and a configuration built
against a stale serial is `cancelled`.

**Why**: output management is the last P0 XFCE-parity gap
(`todo/decisions/qdwin-output-management.md`). qdshell's Display layout tab
is the primary consumer; this exercises the bind→head/mode burst→done→
create_configuration→test/apply→succeeded/failed/cancelled round-trip
headlessly, independent of qdshell. The atomic apply + serial-revert path is
load-bearing: the shell's 15-second confirm-or-revert depends on apply being
all-or-nothing and on stale configs being rejected rather than silently
half-applied.

**Non-visual**: asserts on the probe's exit code (it re-reads the head set
and the configuration reply, failing nonzero on a mismatch) and on the
compositor's `zwlr_output_manager bound (serial=...)` log line. The
window-relayout side of an apply is covered by the VM GUI scenario.

## Setup

```bash
HT=tests/host
. "$HT/lib.sh"
PROBE="$QDWIN_INSTALL/bin/qdwin-output-probe"
ID=12-output
$HT/start.sh $ID --no-shell --no-terminal >/dev/null
RUNTIME=$(ht_runtime $ID); SOCK=$(ht_socket $ID); WLOG=$(ht_log_weston $ID)
WPID=$(ht_pid_load $ID weston)
run() { XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PROBE" "$@"; }
```

> The headless backend advertises a single output ("headless") with one
> mode, so S1 asserts `--expect-heads=1`. On a multi-output backend the
> count differs; drop `--expect-heads` and assert only `heads >= 1`.

## Steps

### S1 — a freshly bound manager enumerates the head/mode set

```bash
if run --expect-heads=1; then RC=0; else RC=$?; fi
if [ "$RC" -eq 0 ]; then echo "S1 PASS"; else echo "S1 FAIL (exit=$RC)"; fi
```

**Assert (S1):** `RC == 0` — the manager advertised exactly 1 head with at
least one mode, its enabled/position/current-mode state, and a `done`
carrying a serial. The probe exits nonzero on a count mismatch or protocol
error. HARD.

### S2 — a configuration test against the current serial succeeds

```bash
if run --test; then RC=0; else RC=$?; fi
if [ "$RC" -eq 0 ]; then echo "S2 PASS"; else echo "S2 FAIL (exit=$RC)"; fi
```

**Assert (S2):** `RC == 0` — building a config (enable + reposition head 0)
against the current serial and calling `test` produced exactly one
`succeeded`. HARD.

### S3 — an apply against the current serial succeeds and bumps the serial

```bash
if run --apply; then RC=0; else RC=$?; fi
if [ "$RC" -eq 0 ]; then echo "S3 PASS"; else echo "S3 FAIL (exit=$RC)"; fi
```

**Assert (S3):** `RC == 0` — `apply` produced one `succeeded`; the
subsequent `done` re-published the current configuration with a bumped
serial (visible in the weston log: a second `zwlr_output_manager bound`
serial value, and the next probe run reads serial+1). HARD.

### S4 — an apply against a stale serial is cancelled (revert-safety)

```bash
if run --bad-serial; then RC=0; else RC=$?; fi
if [ "$RC" -eq 0 ]; then echo "S4 PASS"; else echo "S4 FAIL (exit=$RC)"; fi
```

**Assert (S4):** `RC == 0` — a config built against `serial+1` (a serial the
compositor never issued) was answered with `cancelled`, never applied. This
is the guard that an outdated client view (e.g. after a hotplug) cannot
clobber the layout. HARD.

### S5 — compositor stays alive after apply

```bash
if kill -0 "$WPID" 2>/dev/null; then echo "S5 PASS"; else echo "S5 FAIL (weston died)"; fi
```

**Assert (S5):** `kill -0 weston` succeeds — the apply/relayout path did not
crash the compositor. HARD.

## Teardown

```bash
$HT/stop.sh $ID
```

## Pass criteria

S1 (`RC==0`, 1 head), S2 (test succeeded), S3 (apply succeeded), S4
(bad-serial cancelled), and S5 (weston alive) all hold.

## Known-broken-if

- S1 `heads=0` → the manager bound but sent no heads. Check
  `qdwin_om_manager_create_heads` iterates `compositor->output_list` and
  that `bind_output_manager` sends `done`.
- S2/S3 `failed` instead of `succeeded` → validation rejected the layout, or
  a live `weston_output_*` call failed mid-apply (the rollback path then
  reverts and answers `failed`). Check `qdwin_om_config_realize`.
- S4 `succeeded`/`failed` instead of `cancelled` → the serial check in
  `qdwin_om_config_apply` is missing or compares the wrong field.
- S5 weston dies → likely a NULL deref in the apply path or a
  double-free in the configuration/head/mode resource destructors. Check the
  inert-marking in `qdwin_om_manager_destroy_heads` and the
  `qdwin_om_*_resource_destroy` guards.
```
