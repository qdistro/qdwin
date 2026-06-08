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

# §P10: dual-target build option for the SPICE-retirement migration.
#
#   role=host   — default. Full qdwin: locker (qdwin_locker_v1), the
#                 nested-manager (qdwin_nested_manager_v1), and the
#                 nested-pixelfeed consumer-bind path. KMS/DRM backend
#                 selection is unchanged (weston picks per --backend).
#                 The host build is byte-identical to the pre-P10 build:
#                 we only add a single `-DQDWIN_ROLE_HOST` define which
#                 doesn't gate any code paths in this mode.
#
#   role=guest  — stripped variant intended to run as the in-VM nested
#                 compositor for tier-4-guest. Compiles out the locker
#                 global and the nested-manager global (the guest is not
#                 the locker target; the guest is not itself nesting).
#                 qdwin_shell_v1 stays advertised because qdwin-bystander
#                 --connect --forward-all-toplevels relies on it for
#                 inner-toplevel enumeration (see plan-too-thin tripwire
#                 #2 in plan2/tasks/P10-tier4-guest-image-nested-qdwin.md).
#
# Spec: plan2/research/spice-retirement/00-overview.md §"nested qdwin in
# the guest"; plan2/tasks/P10-tier4-guest-image-nested-qdwin.md §Phase A.
qdwin_role = get_option('role')
if qdwin_role == 'guest'
  add_project_arguments('-DQDWIN_ROLE_GUEST=1', language: 'c')
else
  add_project_arguments('-DQDWIN_ROLE_HOST=1', language: 'c')
endif

dep_libweston = dependency('libweston-14', required: false)
if not dep_libweston.found()
  dep_libweston = dependency('libweston-13')
endif
dep_wayland_server = dependency('wayland-server')
dep_wayland_scanner = dependency('wayland-scanner', native: true)
# v28: set_pointer_config applies the libinput_device_config_* API to the
# DRM/libinput backend's input devices. Header + lib come from libinput.
dep_libinput = dependency('libinput')

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
# Client header for the activation negative-path probe (qdwin-activation-probe).
# The global is server-side in qdwin-shell.so; the probe is a plain client.
xdg_activation_client_header = custom_target(
  'xdg-activation-v1-client-header',
  input: xdg_activation_xml,
  output: 'xdg-activation-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
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

# Bucket A / P1: text-input-v3. Open, app-facing IME text-input plane so
# GTK/Qt/Chromium see zwp_text_input_manager_v3. Foundation only — enter/leave
# focus wiring + buffered enable; inert until an input-method-v2 IME exists (the
# input-method side is gated on a separate identity decision, see
# todo/issues/qdwin/app-compat-protocol-gaps.md P1).
text_input_xml = files(
  wayland_protocols_dir / 'unstable/text-input/text-input-unstable-v3.xml',
)
text_input_server_header = custom_target(
  'text-input-v3-server-header',
  input: text_input_xml,
  output: 'text-input-unstable-v3-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
text_input_protocol_code = custom_target(
  'text-input-v3-private-code',
  input: text_input_xml,
  output: 'text-input-unstable-v3-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# v24 workspaces: ext-workspace-v1 (server). qdwin implements the standard
# workspace protocol so any taskbar (qdshell + others) can list/activate
# workspaces. The window->workspace mapping the bar needs for occupancy is
# carried by qdwin_shell_v1.toplevel_workspace (a sidecar), since
# ext-workspace-v1 is window-agnostic. See
# todo/decisions/qdwin-workspaces-ext-protocol.md.
ext_workspace_xml = files(
  wayland_protocols_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
)
ext_workspace_server_header = custom_target(
  'ext-workspace-v1-server-header',
  input: ext_workspace_xml,
  output: 'ext-workspace-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
ext_workspace_protocol_code = custom_target(
  'ext-workspace-v1-private-code',
  input: ext_workspace_xml,
  output: 'ext-workspace-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
# Client header for the host workspace probe (qdwin-workspace-probe).
ext_workspace_client_header = custom_target(
  'ext-workspace-v1-client-header',
  input: ext_workspace_xml,
  output: 'ext-workspace-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

# Output (display) management: wlr-output-management-unstable-v1. qdwin
# implements the standard wlroots output-management protocol (manager +
# head + mode + configuration objects) so the Display layout tab in qdshell
# (and any other wlr-output-management client, e.g. wlr-randr / kanshi) can
# enumerate heads/modes and apply an ATOMIC layout with test/apply + revert.
# Unlike ext-workspace-v1 this protocol is NOT in wayland-protocols (it is a
# wlr-protocols extension), so the xml is vendored under qdwin/qdwin/ alongside
# the other private protocol XMLs and installed via the qdistro-protocols
# pkg-config package (single source of truth — consumers like the qdshell
# qml-plugin discover it there, never by copying).
# See todo/decisions/qdwin-output-management.md.
wlr_output_mgmt_xml = files('qdwin/wlr-output-management-unstable-v1.xml')
wlr_output_mgmt_server_header = custom_target(
  'wlr-output-management-v1-server-header',
  input: wlr_output_mgmt_xml,
  output: 'wlr-output-management-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
wlr_output_mgmt_protocol_code = custom_target(
  'wlr-output-management-v1-private-code',
  input: wlr_output_mgmt_xml,
  output: 'wlr-output-management-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
# Client header for the host output probe (qdwin-output-probe).
wlr_output_mgmt_client_header = custom_target(
  'wlr-output-management-v1-client-header',
  input: wlr_output_mgmt_xml,
  output: 'wlr-output-management-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
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
# Client header for the secctx bind-gate probe (qdwin-secctx-probe). The
# manager global is server-side in qdwin-shell.so, but the probe is a
# plain Wayland client, so it needs the client-side stubs.
security_context_client_header = custom_target(
  'security-context-v1-client-header',
  input: security_context_xml,
  output: 'security-context-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
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
python3 = find_program('python3')

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
    text_input_server_header,
    text_input_protocol_code,
    ext_workspace_server_header,
    ext_workspace_protocol_code,
    wlr_output_mgmt_server_header,
    wlr_output_mgmt_protocol_code,
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
                 dep_xcursor, dep_libinput],
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

# Secctx bind-gate probe. Binds wp_security_context_manager_v1 and reports
# (via exit code) whether qdwin's launcher gate accepted or refused it;
# with --commit it also exercises create_listener + commit. Driven by
# tests/host/06-secctx-bind-gate.md.
executable('qdwin-secctx-probe',
  sources: [
    'test-client/qdwin-secctx-probe.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
    security_context_client_header,
    security_context_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# Locker bind-gate probe. Binds qdwin_locker_v1 + bind_as_locker and reports
# (via exit code) whether qdwin's uid gate accepted or refused it; --double-bind
# and --rebind exercise the XML's "one locker at a time" semantics. Driven by
# tests/host/07-locker-bind-gate.md.
executable('qdwin-locker-probe',
  sources: [
    'test-client/qdwin-locker-probe.c',
    qdwin_locker_client_header,
    qdwin_locker_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# ext-workspace-v1 server probe. Binds ext_workspace_manager_v1, syncs the
# workspace set, and (with --activate=K) switches + re-reads the active
# index. Also covers the bind-time group→output_enter association
# (--expect-output-enter) and the atomic-commit staging of
# create_workspace+activate (--batch-activate=K). Driven by
# tests/host/11-workspace-protocol.md and 15-ext-workspace-batch.md.
executable('qdwin-workspace-probe',
  sources: [
    'test-client/qdwin-workspace-probe.c',
    ext_workspace_client_header,
    ext_workspace_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# v27 workspace-NAME parity probe. Binds qdwin_shell_v1 (as the shell) AND
# ext_workspace_manager_v1 (as any third-party bar would), pushes a custom
# name via set_workspace_name, and asserts the name is echoed back through
# the standard ext_workspace_handle_v1.name event — and (with
# --expect-revert) that an empty name reverts to the positional default.
# Driven by tests/host/21-ext-workspace-names.md.
executable('qdwin-wsname-probe',
  sources: [
    'test-client/qdwin-wsname-probe.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
    ext_workspace_client_header,
    ext_workspace_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# v28 input-config conformance probe. Binds qdwin_shell_v1 (as the shell),
# asserts the global advertises >= v28, and pushes a valid then a
# deliberately out-of-range set_pointer_config / set_key_repeat snapshot,
# proving the compositor accepts the new requests and clamps/normalises
# bogus values server-side instead of raising a protocol error.
# Driven by tests/host/22-input-config.md.
executable('qdwin-input-probe',
  sources: [
    'test-client/qdwin-input-probe.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# wlr-output-management-v1 server probe. Binds zwlr_output_manager_v1, syncs
# the head/mode set, prints "heads=N", and (with --apply / --test) builds a
# configuration and exercises the atomic apply/test + succeeded/failed/
# cancelled path. Driven by tests/host/12-output-protocol.md.
executable('qdwin-output-probe',
  sources: [
    'test-client/qdwin-output-probe.c',
    wlr_output_mgmt_client_header,
    wlr_output_mgmt_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# qdwin_nested_v1 negative/gating probe. Binds qdwin_shell_v1 (as the shell)
# + qdwin_nested_manager_v1 on one client, advertises an inner toplevel, and
# exercises the advertise -> nested_proxy_pending -> nested_proxy_decision
# state machine (allow/deny/defer/stale/double), plus the bind-uid gate,
# destroy ordering, and malformed/placeholder advertises. Driven by
# tests/host/16-nested-protocol.md.
executable('qdwin-nested-probe',
  sources: [
    'test-client/qdwin-nested-probe.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
    qdwin_nested_client_header,
    qdwin_nested_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# qdwin_nested_v1 nested-proxy identity probe. Binds qdwin_nested_manager_v1
# and calls advertise_toplevel with a deliberately FOREIGN origin_uid (and
# optionally a foreign-owned input_sink), so the harness can assert (via the
# weston log) that qdwin overrides origin_uid back to the advertising client's
# verified peer uid and refuses an input_sink whose peer uid doesn't match.
# Driven by tests/host/16-nested-identity.md.
executable('qdwin-nested-identity-probe',
  sources: [
    'test-client/qdwin-nested-identity-probe.c',
    qdwin_nested_client_header,
    qdwin_nested_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# Client header for the cursor-shape probe (qdwin-cursor-probe).
cursor_shape_client_header = custom_target(
  'cursor-shape-v1-client-header',
  input: cursor_shape_xml,
  output: 'cursor-shape-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
# cursor-shape-v1's get_tablet_tool_v2 references zwp_tablet_tool_v2, so the
# generated protocol code needs the tablet-v2 interface symbols linked too.
tablet_v2_client_protocol_code = custom_target(
  'tablet-v2-client-private-code',
  input: tablet_v2_xml,
  output: 'tablet-v2-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
cursor_shape_client_protocol_code = custom_target(
  'cursor-shape-v1-client-private-code',
  input: cursor_shape_xml,
  output: 'cursor-shape-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# wp_cursor_shape_v1 negative/edge probe. Binds wl_seat + the cursor-shape
# manager, makes a device via get_pointer, and exercises invalid-shape
# rejection (invalid_shape), valid shapes, a shape burst, and a second
# device (cursor-after-focus-change). Driven by tests/host/17-cursor-shape.md.
executable('qdwin-cursor-probe',
  sources: [
    'test-client/qdwin-cursor-probe.c',
    cursor_shape_client_header,
    cursor_shape_client_protocol_code,
    tablet_v2_client_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# v19 global-hotkey REGISTER-edge probe. Binds qdwin_shell_v1 (v19) +
# bind_as_shell and drives register/unregister/replace/duplicate-across-ids/
# modifier-only no-op; the observable postconditions are weston-log
# register_hotkey/unregister_hotkey lines. The real keypress->hotkey_pressed
# DELIVERY half needs input injection (a seat) and is VM-only. Driven by
# tests/host/19-hotkey-edges.md.
executable('qdwin-hotkey-probe',
  sources: [
    'test-client/qdwin-hotkey-probe.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# Client header + code for the fractional-scale probe (qdwin-fractional-probe).
fractional_scale_client_header = custom_target(
  'fractional-scale-v1-client-header',
  input: fractional_scale_xml,
  output: 'fractional-scale-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
fractional_scale_client_protocol_code = custom_target(
  'fractional-scale-v1-client-private-code',
  input: fractional_scale_xml,
  output: 'fractional-scale-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# wp_fractional_scale_v1 probe. Binds wl_compositor + the fractional-scale
# manager, gets a fractional-scale object on a surface, and asserts the
# initial preferred_scale value (incl. a forced non-integer via
# QDWIN_FRACTIONAL_SCALE), commit dedup, and multi-object broadcast. Driven
# by tests/host/18-fractional-scale.md.
executable('qdwin-fractional-probe',
  sources: [
    'test-client/qdwin-fractional-probe.c',
    fractional_scale_client_header,
    fractional_scale_client_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# xdg-activation negative-path probe. Drives qdwin_activation_activate with an
# unknown/forged token and asserts the compositor drops it without a protocol
# error or crash. Driven by tests/host/09-xdg-activation-gating.md.
executable('qdwin-activation-probe',
  sources: [
    'test-client/qdwin-activation-probe.c',
    xdg_activation_client_header,
    xdg_activation_protocol_code,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# xdg-shell client header/code shared by test clients that need to
# create their own xdg_toplevel(s) on an outer compositor. Declared
# here (before qdwin-bystander) so the bystander's --forward-session
# mode can create the single outer toplevel that represents the whole
# tier-4 VM session on the host compositor (§P10 Shape A).
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

# Long-lived stand-in shell for app-compat scenarios. Binds
# qdwin_shell_v1, releases held-layer per toplevel, and exposes a
# max/restore/min/close/focus command FIFO. Used by the qdwin-apps/
# gui-tests as a minimal alternative to qdshell.
#
# §P10 Shape A: when invoked with --inner-display + --forward-session,
# the bystander maintains two wl_display connections — one to the
# inner (in-guest) qdwin via the named socket, and one to the outer
# compositor via $WAYLAND_DISPLAY (set by waypipe-server to its own
# intercept socket). It creates exactly one outer xdg_toplevel that
# represents the entire guest session, regardless of how many inner
# toplevels exist. xdg-shell client headers are therefore wired in.
executable('qdwin-bystander',
  sources: [
    'test-client/qdwin-bystander.c',
    qdwin_shell_client_header,
    qdwin_shell_protocol_code,
    xdg_shell_client_header,
    xdg_shell_protocol_code_test,
  ],
  dependencies: [dep_wayland_client],
  install: true,
)

# Phase-7 tier-3 test helper: minimal xdg_toplevel with a
# configurable --title. Used by s38 because weston-terminal doesn't
# call xdg_toplevel.set_title at startup, leaving waypipe's
# --title-prefix nothing to prepend. Reusable for any future test
# that needs a controllable-title window.
# Client header for the text-input-v3 probe (qdwin-textinput-probe). The
# manager global is server-side in qdwin-shell.so; the probe is a plain
# client. Reuses text_input_protocol_code (private-code) for the wl_interface
# symbols, same as the server.
text_input_client_header = custom_target(
  'text-input-v3-client-header',
  input: text_input_xml,
  output: 'text-input-unstable-v3-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

# text-input-unstable-v3 probe. Binds zwp_text_input_manager_v3, exercises the
# full request set, and asserts the foundation contract: enter is focus-driven
# (one enter on a mapped+focused surface) and NO IME/done events fire without
# an input-method. Driven by tests/host/21-text-input.md.
executable('qdwin-textinput-probe',
  sources: [
    'test-client/qdwin-textinput-probe.c',
    text_input_client_header,
    text_input_protocol_code,
    xdg_shell_client_header,
    xdg_shell_protocol_code_test,
  ],
  dependencies: [dep_wayland_client],
  install: true,
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

# Test helper: inject a clipboard selection tagged with a qdistro-silo
# security context via wp_security_context_v1.  Used by
# tests/integration/vm/s106-browser-clipboard-gate.sh to trigger
# ClipboardGate.qml's CLIPBOARD_GATE journal lines without a real
# browser running. The binary hand-rolls the wp_security_context_v1
# client interface rather than using a generated header so it compiles
# without an extra wayland-scanner custom_target.
executable('qdwin-test-clipboard-emit',
  sources: [
    'test-client/qdwin-test-clipboard-emit.c',
  ],
  dependencies: [dep_wayland_client],
  install: true,
  install_dir: get_option('bindir'),
)

# --- qdistro-protocols: install the private Wayland XMLs and emit a
# pkg-config file so sibling consumers (qdshell qml-plugin, qdistro
# daemons, qdlocker bindings) discover them the same way qdwin itself
# discovers wayland-protocols. Replaces the prior `../../qdwin/qdwin/*.xml`
# sibling-checkout paths in those consumers' build files. See
# qdistro/tests/protocol/test_no_duplicate_protocol_xmls.py for the
# duplicate-XML guard that enforces single-source-of-truth.
qdistro_protocols_dir = get_option('datadir') / 'qdistro' / 'protocols'
install_data(
  [
    'qdwin/qdwin-shell-v1.xml',
    'qdwin/qdwin-locker-v1.xml',
    'qdwin/qdwin-nested-v1.xml',
    'qdwin/wlr-layer-shell-unstable-v1.xml',
    'qdwin/wlr-output-management-unstable-v1.xml',
  ],
  install_dir: qdistro_protocols_dir,
)

pkg = import('pkgconfig')
pkg.generate(
  name: 'qdistro-protocols',
  description: 'qdistro private Wayland protocol XMLs (owned by qdwin)',
  version: meson.project_version(),
  variables: [
    'pkgdatadir=' + get_option('prefix') / qdistro_protocols_dir,
  ],
  uninstalled_variables: [
    'pkgdatadir=' + meson.current_source_dir() / 'qdwin',
  ],
)

test('xdg-activation-policy',
  python3,
  args: [
    files('qdwin/test_xdg_activation_policy.py'),
    files('qdwin/qdwin.c'),
  ],
)

test('nested-identity-policy',
  python3,
  args: [
    files('qdwin/test_nested_identity_policy.py'),
    files('qdwin/qdwin.c'),
  ],
)

# RDP-headless focus-listener rebind invariants. Driven by
# tests/host/20-rdp-focus-rebind.md (live VM half is PENDING residue).
test('rdp-focus-rebind',
  python3,
  args: [
    files('qdwin/test_rdp_focus_rebind.py'),
    files('qdwin/qdwin.c'),
  ],
)

# text-input-unstable-v3 foundation invariants (Bucket A / P1): manager open
# at v1, focus-driven client-scoped enter/leave, and the inert "no IME events
# without an input-method" contract. Driven by tests/host/21-text-input.md;
# the live enter/leave half is seat/VM-gated (headless has no wl_seat).
test('text-input-foundation',
  python3,
  args: [
    files('qdwin/test_text_input.py'),
    files('qdwin/qdwin.c'),
  ],
)

# v27 ext-workspace-v1 workspace-NAME parity invariants
# (qdwin_shell_v1.set_workspace_name). Driven by
# tests/host/21-ext-workspace-names.md; the live-VM half (a real
# third-party ext-workspace bar showing the user's names) is PENDING.
test('workspace-name-policy',
  python3,
  args: [
    files('qdwin/test_workspace_name_policy.py'),
    files('qdwin/qdwin.c'),
  ],
)

# v28 live input-config invariants (qdwin_shell_v1.set_pointer_config /
# set_key_repeat). Driven by tests/host/22-input-config.md; the live-VM
# half (a real mouse/touchpad / physical keyboard reacting) is PENDING.
test('input-config-policy',
  python3,
  args: [
    files('qdwin/test_input_config_policy.py'),
    files('qdwin/qdwin.c'),
  ],
)

# Worktree-aware vm-exec / workspace lookup in the GUI agent test harness.
# Builds throwaway normal + .worktrees/ layouts and sources the real helper
# files to assert vm-exec resolves correctly from a worktree. Headless.
test('vm-exec-lookup',
  python3,
  args: [
    files('qdwin/test_vm_exec_lookup.py'),
    files('tests/gui/qdwin-helpers.sh'),
    files('tests/apps/qdwin-apps-helpers.sh'),
  ],
)
