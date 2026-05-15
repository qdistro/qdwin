# Locker protocol wiring (`qdwin_locker_v1`)

Adds a second uid-filtered private global so the screen locker
(`qdlocker`) is a peer process to the shell (`qdshell`), not a
subsystem of it. Rationale and shape live in
`qdwin/qdwin-locker-v1.xml`; this doc describes the C-side work in
`qdwin/qdwin.c`.

## Pre-existing state in qdwin.c

The lock-surface plumbing already exists on `qdwin_shell_v1`:

- `qdwin_handle_attach_lock_surface` at qdwin.c:4946
- `qdwin_handle_set_locked` at qdwin.c:4996
- `qdwin_shell_v1_send_locked_changed` calls at qdwin.c:4736, 4775, 5016
- Shell-side bind at `bind_qdwin_shell` (qdwin.c:5461)
- Global registration at qdwin.c:11538 (`wl_global_create(... qdwin_shell_v1_interface, 21, ...)`)

The state machine — `qdwin->lock_surface_view`, `qdwin->locked`,
the LOCK layer — is already correct. We are adding a second entry
point that drives the same state, not duplicating state.

## Changes

1. **Config**: add `qdwin->allowed_locker_uid`. Source: same admin
   config path that sets `allowed_uid`. Default: equal to
   `allowed_uid` (single-tenant assumption from
   `qdistro/doc/sessions.md:4-14`). Settable separately so future
   sandboxed-locker work can put the locker behind its own uid.

2. **Global registration** (next to qdwin.c:11538):

   ```c
   qdwin->locker_global = wl_global_create(
       ec->wl_display,
       &qdwin_locker_v1_interface,
       1, qdwin, bind_qdwin_locker);
   ```

3. **`bind_qdwin_locker`** — mirror of `bind_qdwin_shell` at
   qdwin.c:5461 but checking `allowed_locker_uid`. Rejects a second
   bind via the `already_bound` enum.

4. **Locker resource implementation** — a `struct
   qdwin_locker_v1_interface` vtable:

   - `bind_as_locker`: stash `qdwin->locker_resource = resource`,
     send `ready(initially_locked = qdwin->locked)`.
   - `attach_lock_surface`: reuse `qdwin_handle_attach_lock_surface`
     body. Surface state moves into a shared helper called by both
     the shell and locker handlers (during migration both paths
     accept it; post-migration only the locker does).
   - `set_locked`: reuse `qdwin_handle_set_locked` body. The
     `locked_changed` send needs to fan out to both the shell
     resource (`qdwin_shell_v1_send_locked_changed`) and the locker
     resource (`qdwin_locker_v1_send_locked_changed`).
   - `lock_acknowledged`: log only; used by tests.

5. **Event sources to redirect to the locker**:

   - `lock_requested` — currently fired on `qdwin_shell_v1` from
     the Ctrl+Alt+L hotkey (qdwin.c around the hotkey channel,
     since v7). Add a parallel send on
     `qdwin_locker_v1_send_lock_requested(qdwin->locker_resource, reason)`.
   - `idle_lock_hint` — same shape; folds into
     `lock_requested(reason)` on the locker side. The shell side
     keeps `idle_lock_hint` for source compatibility, but the
     locker only gets the unified event.
   - `overlay_key` with `role=2` (locker) — qdwin.c overlay-key
     dispatch must check whether a locker is bound; if yes, route
     the keystroke to `qdwin_locker_v1_send_overlay_key(locker_resource, sym, utf8)`
     instead of the shell's overlay_key event. **Important**: this
     is the security boundary that means the typed password never
     reaches the shell process.

6. **Shell-side deprecation** of `attach_lock_surface` /
   `set_locked` / `lock_requested` on `qdwin_shell_v1`: not yet.
   Keep them functional during qdlocker rollout. Once
   `qdshell/Modules/LockScreen/*` is deleted (sibling-repo
   migration), bump `qdwin_shell_v1` and remove the shell-side
   handlers in the same commit.

## Lifecycle invariants

- The compositor must boot locked (`qdwin->locked = 1`). It already
  does — see the boot-locked path in sessions.md:111-132.
- If the locker disconnects while locked, the LOCK layer stays
  black (no surface to render). The shell continues to receive
  `locked_changed` so it knows not to draw chrome. A fresh locker
  bind re-attaches a surface and the screen comes back.
- A second `bind_as_locker` from a different client at the locker
  uid is the "old process is dead" path: tear down the old
  resource (it will get a Wayland protocol error on next dispatch
  and exit), accept the new one. Implemented in the
  `already_bound` enum branch.

## Tests to add under `qdwin/tests/`

- A `qdwin-locker-probe` test client (mirror of
  `qdwin-probe`) that binds `qdwin_locker_v1`, attaches a dummy
  surface, calls `set_locked(1)` / `set_locked(0)`, checks the
  `locked_changed` events.
- A negative-uid test: bind from a non-locker uid → expect
  implementation error.
- A handover test: bind locker A → bind locker B → A's
  resource gets the protocol-defined teardown.
