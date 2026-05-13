# qdwin

A libweston shell plugin that hosts the qdistro desktop. Single-seat,
single-user, designed for one trusted shell client (typically
[qdshell](https://codeberg.org/qdistro/qdshell)) plus arbitrary
sandboxed application clients placed by the shell.

qdwin is the compositor half of [qdistro](https://codeberg.org/qdistro/qdistro)
but is published as a separate project: nothing in qdwin assumes the
qdistro broker, the qdshell QML stack, or the qdistro userland is
present. A different shell can adopt it.

## What's in here

- `qdwin/` — the shell plugin: `qdwin.c` + nested-compositor client
  glue + protocol XML.
- `libweston-vendored/` — libweston 14 with a single local patch
  (`0001-allow-null-parent-xdg-popup.patch`) that lifts the assertion
  blocking root-level popups. Vendored because the patch hasn't
  landed upstream.
- `test-client/` — minimal Wayland clients used by the host and VM
  tests: `qdwin-probe`, `qdwin-bystander`, plus xdg-toplevel and
  clipboard helpers.
- `tests/host/` — host-side scenarios (markdown playbooks + shell
  helpers + screenshot/ctrl helpers).
- `tests/gui/` — VM scenarios driving qdwin via virsh send-key and
  the qemu guest agent.
- `tests/apps/` — VM scenarios validating real toolkits (GTK, Qt,
  Electron, FLTK, Tk, Java Swing, wxWidgets, imlib2).
- `meson.build` — top-level build.

## What's NOT in here

The qdistro userland — broker, polkit agent, SDK, vault, admin app,
etc. — lives in the [qdistro umbrella repo](https://codeberg.org/qdistro/qdistro).
The qdshell QML lives in [qdshell](https://codeberg.org/qdistro/qdshell).

Several daemons that consume qdwin protocols (`qdistro-cursor-sprites`,
`qdistro-nested-pixelfeed`, `qdistro-secctx-exec`, `qdistro-tier1-exec`,
`qdistro-forward`) ship in the umbrella repo. They consume qdwin's XML
via the system `wayland-protocols` directory once qdwin is installed.

## Build

```sh
meson setup build
meson compile -C build
meson install -C build      # installs qdwin-shell.so under libdir/weston/
```

Then launch weston with the qdwin shell:

```sh
weston --shell=qdwin-shell.so
```

For the vendored libweston (only needed if the distro libweston isn't
patched), see `libweston-vendored/README.md`.

## Protocol

qdwin exposes a private protocol (`qdwin_shell_v1`, currently at
version 21) plus a nested-compositor protocol (`qdwin_nested_v1`) for
proxying second-tier compositors. Public protocols supported:
xdg-shell, xdg-activation, ext-idle-notify, idle-inhibit-unstable-v1,
cursor-shape-v1, fractional-scale-v1, primary-selection-unstable-v1,
security-context-v1, zwlr-layer-shell-v1.

See [doc/protocol.md](doc/protocol.md), [doc/architecture.md](doc/architecture.md),
and [doc/nested.md](doc/nested.md).

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).

`libweston-vendored/` carries upstream's MIT license (see
`libweston-vendored/COPYING`). The `0001-allow-null-parent-xdg-popup.patch`
modifies libweston source and is offered under MIT to match upstream.
