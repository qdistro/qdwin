# 05 — GTK4 native Wayland app: gnome-text-editor

**Acceptance criterion:** a GTK4 application running native-Wayland
(no XWayland) launches, accepts keystrokes that appear in the
document, maximises and restores cleanly. Smoke test for the GTK4
toolkit on qdwin.

## Setup

```bash
source phase1/gui-tests/qdwin-apps/qdwin-apps-helpers.sh
qdwin_apps_set_vm "${VMNAME}"
qdwin_apps_session_up || { echo "FAIL: bystander/weston not healthy"; exit 1; }
qdwin_apps_kill_all
```

## Steps

### Step 1 — launch

```bash
qdwin_apps_launch gnome-text-editor "gnome-text-editor"
sleep 8
qdwin_apps_screenshot /tmp/05-step1-launched.png
```

**Assert (1.1):** screenshot shows the gnome-text-editor window with
"New Document" titlebar, an `Open` button at the top-left, hamburger
menu at the top-right, and an empty editing area.
**Assert (1.2):** bystander log shows
`toplevel_added handle=<N> ... xwayland=0` (it's native Wayland).

### Step 2 — type "qdwin"

```bash
qdwin_apps_type "qdwin"
sleep 1
qdwin_apps_screenshot /tmp/05-step2-typed.png
```

**Assert (2.1):** screenshot shows `qdwin` rendered in the editing
area (cursor blinking after the `n`). The titlebar updates to show
`qdwin` (the auto-derived document title) with a small `Draft` label.

### Step 3 — maximise

```bash
qdwin_apps_ctl "maxlast"
sleep 2
qdwin_apps_screenshot /tmp/05-step3-max.png
```

**Assert (3.1):** screenshot shows the editor filling the full
1280×800 output. Header bar is now the only chrome strip at the top.

### Step 4 — restore

```bash
qdwin_apps_ctl "restorelast"
sleep 2
qdwin_apps_screenshot /tmp/05-step4-restore.png
```

**Assert (4.1):** screenshot shows the editor back at approximately
its launch size (~700×550 px window, centred-ish, with black margin).
The typed text `qdwin` is still visible.

## Cleanup

```bash
qdwin_apps_ctl "close" || qdwin_apps_kill_all
```

## Pass criteria

- All four screenshots match assertions.
- "qdwin" appears in the document at step 2 (proves wl_keyboard
  delivery).
- Maximise/restore round-trip preserves the typed text.

## Known failure modes

- **Black canvas** — toplevel_added arrives but content stays black.
  GTK4 + qdwin pixman renderer would only fail this way if GTK4 went
  through a wl_egl_window path; the default GTK4 backend is shm so
  this should work. If it doesn't, file under
  .
- **dbus activation race** — gnome-text-editor exits immediately with
  "cannot open display" because GApplication registered via dbus
  without inheriting WAYLAND_DISPLAY. Track via
   item 2.
