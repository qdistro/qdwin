# Protocols

qdwin exposes two private protocols (`qdwin_shell_v1`, currently
at version 21, and `qdwin_locker_v1` v1 — see [locker.md](locker.md)),
one helper protocol for nested compositors (`qdwin_nested_v1`, see
[nested.md](nested.md)), and a curated set of public Wayland
protocols.

## qdwin_shell_v1 (private, single-client)

The shell-management protocol. One client binds; every other bind
attempt is filtered out at `wl_global` registration time using
libweston's bind-filter callback against the connecting peer's uid.

### Sub-interfaces

| Interface | Purpose |
| --- | --- |
| `qdwin_shell_v1` | top-level shell handle |
| `qdwin_background_v1` | wallpaper surface bound via `attach_background` (since v18) |
| `qdwin_panel_v1` | panel/bar surface bound via `attach_panel` (since v3); reserves a work-area edge |
| `qdwin_lock_surface_v1` | full-display lock surface bound via `attach_lock_surface` (since v6) |
| `qdwin_launcher_v1` | app-launcher surface bound via `attach_launcher` (since v5) |
| `qdwin_notification_v1` | notification toast attached via `attach_notification` (since v4) |
| `qdwin_popup_v1` | shell-managed popups (right-click menus, chrome) via `show_popup` |
| `qdwin_view_stream_v1` | per-toplevel PipeWire view stream (subscribe + frame events) |
| `qdwin_stream_input_v1` + handle | input forwarding into nested-compositor proxies |

### Versioning policy

Every protocol change bumps the integer version. Three edits are
required for each bump:

1. `qdwin/qdwin-shell-v1.xml` — the `since="N"` annotation on the new
   request/event, and the top-level `version="N"` attribute on the
   interface.
2. `qdwin/qdwin.c` — the `version:` argument to `weston_desktop_create`
   (or wherever the global is registered) AND the version check in
   each `bind` handler that conditionally sends a new event.
3. `qdwin/qdwin.c` again — the `wl_global_create` version argument
   when registering the global on the wl_display.

Missing any one of the three leaves the global pinned at the previous
version even though the XML and handlers claim the new one. The bug
is silent — clients bind the lower version and the new requests are
unreachable. This is documented because it has cost real time in the
past.

### Cross-client policy gates

Several events round-trip a policy decision back to the shell client
before qdwin acts:

- `nested_proxy_pending` / `nested_proxy_decision` (since v8) —
  admit-or-deny for a new nested compositor toplevel.
- `activation_pending` / `activation_decision` (since v12) —
  admit-or-deny for an `xdg-activation` token-driven focus request.
- `data_offer_receive_pending` / `data_offer_receive_decision` (since
  v15) — admit-or-deny each cross-client clipboard receive. The
  decision carries the destination MIME type, the source client's
  security-context tag, and the destination client's tag.

Every gate is fail-safe: timing out the shell's response defaults
to deny.

### Hotkey channel (since v19)

`register_hotkey(modifiers, keysym, slot)` registers a global hotkey
that fires as `hotkey_pressed(slot)`. qdwin owns the keymap and the
seat keyboard grab; the shell client wires hotkey slots into its UI
intents (open launcher, switch workspace, etc.) but never sees raw
keystrokes.

## Public Wayland protocols supported

Built-in, registered unconditionally:

- **xdg-shell** (stable) — via libweston-desktop.
- **wl_seat / wl_output / wl_subcompositor / wl_data_device_manager**
  — libweston defaults.

Built-in, registered by qdwin-shell.so:

- **zwlr_layer_shell_v1** (v5) — vendored XML in
  `qdwin/wlr-layer-shell-unstable-v1.xml`. The protocol was never
  accepted into wayland-protocols upstream; the XML is the de-facto
  reference. Required so external clients (waybar, fuzzel, mako,
  Quickshell-as-bar, etc.) can place layered surfaces.
- **xdg-activation-v1** — focus tokens, gated by qdwin policy.
- **ext-idle-notify-v1** + **idle-inhibit-unstable-v1** — idle
  detection for screen-locking and inhibit hints.
- **cursor-shape-v1** — CSS-style cursor shape requests; qdwin maps
  to libXcursor theme sprites.
- **wp_fractional_scale_v1** — per-surface fractional scale.
- **primary-selection-unstable-v1** — middle-click clipboard.
- **wp_security_context_v1** — sandbox-engine identity tag carried
  through `wl_socket_create_listener` mode.
- **tablet-v2** — referenced by `cursor-shape-v1`; not used standalone.

### Security posture: layer-shell

`zwlr_layer_shell_v1` is registered as an unconditional public global
(advertised to all clients via `wl_registry`), but the bind handler
(`bind_qdwin_layer_shell`) gates access: only the bound shell client
(qdshell, matched by client pointer or pid+uid) or a client running as
`allowed_uid` (before the shell binds) may successfully bind.
Unauthorized bind attempts are rejected with
`wl_client_post_implementation_error`.

Once bound, `EXCLUSIVE` keyboard interactivity is granted to any layer
surface that requests it (e.g. swaylock-style overlays, launcher grabs).
Note: before qdshell binds, any client running as `allowed_uid`
(defaults to compositor euid) can bind layer-shell, so the window
between compositor startup and shell bind is a wider-trust period.
After the shell binds, only qdshell (or same pid+uid) may bind.

The global is advertised broadly so that third-party layer-shell clients
(waybar, fuzzel, mako, gtk-layer-shell bars) see it in `wl_registry`.
In practice, these clients will fail to bind unless they run as the
shell or `allowed_uid`; the advertisement exists for protocol discovery
rather than access.

**Remaining hardening (deferred to production milestone):**

- Hide the global from non-shell clients via a `wl_global` filter
  (analogous to the secctx global filter) so untrusted clients cannot
  even enumerate it.
- Consider per-request policy for `EXCLUSIVE` + high-z layers, gating
  through the shell/broker decision channel rather than granting at bind
  time.
- Cross-silo policy enforcement happens downstream in qdshell/broker.

See `todo/qdwin-codex-review.md` finding #1 for the original review.

### Security posture: wp_security_context_v1

`wp_security_context_v1` tags (`sandbox_engine`, `app_id`,
`instance_id`) are **advisory routing metadata, not trusted identity**.
Any client that can bind the manager can set arbitrary tag values.

Bind-time mitigation: `bind_qdwin_secctx_manager` restricts the manager
global to the shell client (qdshell) or `allowed_uid`, and a global
filter (`qdwin_secctx_global_filter`) hides the global from
already-sandboxed clients so nesting is impossible. The env var
`QDWIN_SECCTX_OPEN=1` disables the bind gate for developer workflows.

**Downstream verification (design intent):** qdwin forwards secctx tags
to the shell, but the authoritative identity check happens in
qdshell/broker, which independently verifies:

- `/proc/<pid>/attr/current` (SELinux label)
- `/proc/<pid>/exe` (executable path)
- pid start time (to detect pid reuse)
- uid match

Cgroup and namespace identity are collected for audit/display but are
not currently used as verification inputs in the broker's identity
check. The compositor deliberately does not duplicate this verification;
treating secctx as a routing hint rather than an authentication
credential is the intended layering.

Reference: `qdistro/doc/isolation-tiers.md`, `qdistro/doc/permissions.md`.

## Source of truth

`qdwin/qdwin-shell-v1.xml` is the single authoritative description
of the private protocol. Doc files in this directory describe
*policy* — what each interface is for and how the trust model
shapes it. Wire-level details (request/event signatures, since
versions, fixed-point precisions) belong in the XML, not here.
