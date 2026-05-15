project('qdwin', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
  ],
)

# Keep this build simple: use pkg-config for everything, so the plugin
# tracks whatever libweston version the distro ships.
cc = meson.get_compiler('c')

dep_libweston = dependency('libweston-14')
dep_wayland_server = dependency('wayland-server')
dep_wayland_scanner = dependency('wayland-scanner', native: true)

# Private protocol codegen.
wayland_scanner = find_program(
  dep_wayland_scanner.get_variable(pkgconfig: 'wayland_scanner'),
  native: true,
)

proto_xml = files('qdwin/qdwin-shell-v1.xml')

qdwin_shell_server_header = custom_target(
  'qdwin-shell-v1-server-header',
  input: proto_xml,
  output: 'qdwin-shell-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

qdwin_shell_client_header = custom_target(
  'qdwin-shell-v1-client-header',
  input: proto_xml,
  output: 'qdwin-shell-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

qdwin_shell_protocol_code = custom_target(
  'qdwin-shell-v1-private-code',
  input: proto_xml,
  output: 'qdwin-shell-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# qdwin_locker_v1 — private, uid-filtered locker attach protocol.
# Bound by qdlocker (separate process from qdshell). Carries
# attach_lock_surface, set_locked, lock_requested, locked_changed,
# and overlay_key forwarding (locker role, role=2 keystrokes go
# straight to qdlocker rather than via qdshell).
# See doc/locker.md for the bind/dispatch wiring.
locker_proto_xml = files('qdwin/qdwin-locker-v1.xml')

qdwin_locker_server_header = custom_target(
  'qdwin-locker-v1-server-header',
  input: locker_proto_xml,
  output: 'qdwin-locker-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

qdwin_locker_client_header = custom_target(
  'qdwin-locker-v1-client-header',
  input: locker_proto_xml,
  output: 'qdwin-locker-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

qdwin_locker_protocol_code = custom_target(
  'qdwin-locker-v1-private-code',
  input: locker_proto_xml,
  output: 'qdwin-locker-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# §6.7: public xdg-activation-v1 protocol for focus-stealing prevention
# and launcher tokens. Sourced from the installed wayland-protocols
# pkgdatadir to avoid vendoring.
dep_wayland_protocols = dependency('wayland-protocols', native: true)
wayland_protocols_dir = dep_wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
xdg_activation_xml = files(
  wayland_protocols_dir / 'staging/xdg-activation/xdg-activation-v1.xml',
)

xdg_activation_server_header = custom_target(
  'xdg-activation-v1-server-header',
  input: xdg_activation_xml,
  output: 'xdg-activation-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

xdg_activation_protocol_code = custom_target(
  'xdg-activation-v1-private-code',
  input: xdg_activation_xml,
  output: 'xdg-activation-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# §6.7 more: ext-idle-notify-v1 (+ idle-inhibit-unstable-v1).
ext_idle_notify_xml = files(
  wayland_protocols_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
)
idle_inhibit_xml = files(
  wayland_protocols_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
)

ext_idle_notify_server_header = custom_target(
  'ext-idle-notify-v1-server-header',
  input: ext_idle_notify_xml,
  output: 'ext-idle-notify-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
ext_idle_notify_protocol_code = custom_target(
  'ext-idle-notify-v1-private-code',
  input: ext_idle_notify_xml,
  output: 'ext-idle-notify-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
idle_inhibit_server_header = custom_target(
  'idle-inhibit-v1-server-header',
  input: idle_inhibit_xml,
  output: 'idle-inhibit-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
idle_inhibit_protocol_code = custom_target(
  'idle-inhibit-v1-private-code',
  input: idle_inhibit_xml,
  output: 'idle-inhibit-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# §6.7 cursor-shape-v1 + fractional-scale-v1.
cursor_shape_xml = files(
  wayland_protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
)
fractional_scale_xml = files(
  wayland_protocols_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
)
# tablet-v2 interface is referenced by cursor-shape-v1.
tablet_v2_xml = files(
  wayland_protocols_dir / 'stable/tablet/tablet-v2.xml',
)
cursor_shape_server_header = custom_target(
  'cursor-shape-v1-server-header',
  input: cursor_shape_xml,
  output: 'cursor-shape-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
cursor_shape_protocol_code = custom_target(
  'cursor-shape-v1-private-code',
  input: cursor_shape_xml,
  output: 'cursor-shape-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
tablet_v2_server_header = custom_target(
  'tablet-v2-server-header',
  input: tablet_v2_xml,
  output: 'tablet-v2-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
tablet_v2_protocol_code = custom_target(
  'tablet-v2-private-code',
  input: tablet_v2_xml,
  output: 'tablet-v2-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
fractional_scale_server_header = custom_target(
  'fractional-scale-v1-server-header',
  input: fractional_scale_xml,
  output: 'fractional-scale-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
fractional_scale_protocol_code = custom_target(
  'fractional-scale-v1-private-code',
  input: fractional_scale_xml,
  output: 'fractional-scale-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# §6.10: wp_security_context_v1 — sandboxed-client identity. Lets
# waypipe (and any other sandbox engine) tag the bridged Wayland
# connection so the compositor can apply per-app policy without
# relying on uid/PEERCRED. spec/02 row 3 + spec/27 revisit trigger #5.
security_context_xml = files(
  wayland_protocols_dir / 'staging/security-context/security-context-v1.xml',
)
security_context_server_header = custom_target(
  'security-context-v1-server-header',
  input: security_context_xml,
  output: 'security-context-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
security_context_protocol_code = custom_target(
  'security-context-v1-private-code',
  input: security_context_xml,
  output: 'security-context-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# §6.7: primary-selection-unstable-v1 — middle-click clipboard.
primary_selection_xml = files(
  wayland_protocols_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
)
primary_selection_server_header = custom_target(
  'primary-selection-unstable-v1-server-header',
  input: primary_selection_xml,
  output: 'primary-selection-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
primary_selection_protocol_code = custom_target(
  'primary-selection-unstable-v1-private-code',
  input: primary_selection_xml,
  output: 'primary-selection-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# Layer-shell (zwlr_layer_shell_v1 v5): vendored from
# Quickshell's repo because wayland-protocols never accepted it
# upstream. Source XML is at qdwin/wlr-layer-shell-unstable-v1.xml
# (407 lines). Lets external Wayland clients (waybar, Quickshell,
# eww, fuzzel, mako, etc.) place panels/notifications/lockscreens on
# fixed compositor layers with anchor + exclusive-zone reservation.
wlr_layer_shell_xml = files('qdwin/wlr-layer-shell-unstable-v1.xml')
wlr_layer_shell_server_header = custom_target(
  'wlr-layer-shell-unstable-v1-server-header',
  input: wlr_layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
wlr_layer_shell_protocol_code = custom_target(
  'wlr-layer-shell-unstable-v1-private-code',
  input: wlr_layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# xdg-shell server-protocol: needed by qdwin-shell.so itself because
# wlr-layer-shell's get_popup request takes an xdg_popup typed object,
# so wayland-scanner emits a reference to xdg_popup_interface in the
# generated wlr-layer-shell-unstable-v1-protocol.c. Without this
# protocol code in the link set, the .so dlopens with
# "undefined symbol: xdg_popup_interface" — neither libweston nor
# weston binary export it (xdg-shell is implemented in libweston-desktop,
# but the wl_interface symbols themselves come from the protocol code).
xdg_shell_server_header_plugin = custom_target(
  'xdg-shell-server-header-plugin',
  input: files(wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml'),
  output: 'xdg-shell-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_protocol_code_plugin = custom_target(
  'xdg-shell-private-code-plugin',
  input: files(wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml'),
  output: 'xdg-shell-plugin-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# xdg-decoration-unstable-v1: lets clients ask whether the compositor
# will decorate. qdshell draws chrome via the private qdwin_shell_v1
# protocol, so the compositor's correct answer is always server_side
# — but clients (notably PyQt6 / Qt-Wayland with Qt's built-in
# bradient CSD plugin) abort during the first QMainWindow when the
# compositor advertises no decoration manager at all. Implementing
# this protocol with an always-server_side response makes those
# clients skip CSD instead. See todo/qdwin-crash.md.
xdg_decoration_xml = files(
  wayland_protocols_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
)
xdg_decoration_server_header = custom_target(
  'xdg-decoration-unstable-v1-server-header',
  input: xdg_decoration_xml,
  output: 'xdg-decoration-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
xdg_decoration_protocol_code = custom_target(
  'xdg-decoration-unstable-v1-private-code',
  input: xdg_decoration_xml,
  output: 'xdg-decoration-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# §6.8 S0: qdwin_nested_v1 — private protocol for nested-compositor
# native-toplevel passthrough. S0 (2026-04-25) ships the XML + stub
# server-side handler that registers the global and logs advertises;
# S1 wires per-toplevel PipeWire streams on the nested side; S2 hooks
# up actual proxy surfaces on the outer side.
qdwin_nested_xml = files('qdwin/qdwin-nested-v1.xml')
qdwin_nested_server_header = custom_target(
  'qdwin-nested-v1-server-header',
  input: qdwin_nested_xml,
  output: 'qdwin-nested-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
qdwin_nested_protocol_code = custom_target(
  'qdwin-nested-v1-private-code',
  input: qdwin_nested_xml,
  output: 'qdwin-nested-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
qdwin_nested_client_header = custom_target(
  'qdwin-nested-v1-client-header',
  input: qdwin_nested_xml,
  output: 'qdwin-nested-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

# §6.7(b): libXcursor for cursor-shape-v1 theme validation + future
# pointer-sprite install. Required at build time.
dep_xcursor = dependency('xcursor')

# §6.8 S1: nested-mode publisher needs wayland-client to bind the
# outer compositor's qdwin_nested_manager_v1 from inside qdwin-shell.so.
# Symbol naming for the dual-side protocol headers (server in qdwin.c,
# client in qdwin-nested-client.c) is kept conflict-free by isolating
# the client header to its own translation unit.
dep_wayland_client = dependency('wayland-client')

qdwin_plugin = shared_library(
  'qdwin-shell',
  sources: [
    'qdwin/qdwin.c',
    'qdwin/qdwin-nested-client.c',
    qdwin_shell_server_header,
    qdwin_shell_protocol_code,
    qdwin_locker_server_header,
    qdwin_locker_protocol_code,
    xdg_activation_server_header,
    xdg_activation_protocol_code,
    ext_idle_notify_server_header,
    ext_idle_notify_protocol_code,
    idle_inhibit_server_header,
    idle_inhibit_protocol_code,
    cursor_shape_server_header,
    cursor_shape_protocol_code,
    tablet_v2_server_header,
    tablet_v2_protocol_code,
    fractional_scale_server_header,
    fractional_scale_protocol_code,
    primary_selection_server_header,
    primary_selection_protocol_code,
    security_context_server_header,
    security_context_protocol_code,
    qdwin_nested_server_header,
    qdwin_nested_protocol_code,
    qdwin_nested_client_header,
    wlr_layer_shell_server_header,
    wlr_layer_shell_protocol_code,
    xdg_shell_server_header_plugin,
    xdg_shell_protocol_code_plugin,
    xdg_decoration_server_header,
    xdg_decoration_protocol_code,
  ],
  dependencies: [dep_libweston, dep_wayland_server, dep_wayland_client,
                 dep_xcursor],
  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / 'weston',
  # Defer symbol resolution until weston dlopens us — screenshooter_create
  # (and other frontend helpers) live in the weston binary, not libweston,
  # and aren't available at link time against the shipped -devel package.
  link_args: ['-Wl,--allow-shlib-undefined', '-Wl,--undefined=screenshooter_create'],
  override_options: ['b_lundef=false'],
)

# Day-2 test client. Wayland-client, binds qdwin_shell_v1, logs the
# hello event or the disconnect, exits.
executable('qdwin-probe',
  sources: [
    'test-client/qdwin-probe.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# Long-lived stand-in shell for app-compat scenarios. Binds
# qdwin_shell_v1, releases held-layer per toplevel, and exposes a
# max/restore/min/close/focus command FIFO. Used by the qdwin-apps/
# gui-tests as a minimal alternative to qdshell.
executable('qdwin-bystander',
  sources: [
    'test-client/qdwin-bystander.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# Phase-7 tier-3 test helper: minimal xdg_toplevel with a
# configurable --title. Used by s38 because weston-terminal doesn't
# call xdg_toplevel.set_title at startup, leaving waypipe's
# --title-prefix nothing to prepend. Reusable for any future test
# that needs a controllable-title window.
xdg_shell_client_header = custom_target(
  'xdg-shell-client-header',
  input: files(wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml'),
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_protocol_code_test = custom_target(
  'xdg-shell-private-code-test',
  input: files(wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml'),
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
executable('qdistro-test-window',
  sources: [
    'test-client/qdistro-test-window.c',
    xdg_shell_client_header,
    xdg_shell_protocol_code_test,
  ],
  dependencies: [dep_wayland_client],
  install: true,
  install_dir: get_option('bindir'),
)

# Clipboard set_selection helper that bypasses wl-clipboard's
# focus-wait. Used by spec/10 e2e bats; see the binary's leading
# comment for why wl-copy doesn't work headlessly.
executable('qdistro-test-clipboard-source',
  sources: [
    'test-client/qdistro-test-clipboard-source.c',
  ],
  dependencies: [dep_wayland_client],
  install: true,
  install_dir: get_option('bindir'),
)

# Clipboard receive helper for the spec/10 v15 wire flow. Creates a
# real xdg_toplevel so qdshell sees toplevel_added(handle) and the
# bats driver can inject-focus to it; binds wl_data_device on the
# only seat; on selection event with a non-NULL offer, calls
# wl_data_offer.receive(--mime, fd) and pipes the result. Closes the
# wl-paste-needs-focus gap that left s53 SKIP-only on headless RDP.
executable('qdistro-test-clipboard-sink',
  sources: [
    'test-client/qdistro-test-clipboard-sink.c',
    xdg_shell_client_header,
    xdg_shell_protocol_code_test,
  ],
  dependencies: [dep_wayland_client],
  install: true,
  install_dir: get_option('bindir'),
)

# The Qdistro.Qdwin QML plugin that binds qdwin_shell_v1 from
# qdshell lives in the qdshell sibling repo (qdshell/qml-plugin/) —
# it reads this directory's qdwin-shell-v1.xml via a relative path
# during its own meson build. See qdshell/qml-plugin/meson.build.
