# qdwin-apps GUI test scenarios — for orchestrator + runner subagents

These scenarios verify **third-party application compatibility** with
the qdwin compositor. Distinct from `phase1/gui-tests/qdwin/`, which
exercises qdshell (panel/launcher/locker). Here qdshell is **not**
running — instead a minimal `qdwin-bystander` C client (built from
`compositor/test-client/qdwin-bystander.c`) plays the role of a v14
shell: it releases the held-layer for each new toplevel and accepts
`max` / `restore` / `min` / `close` / `focus` commands on a FIFO at
`/run/user/1000/qdwin-cmd.fifo`.

These scenarios were authored 2026-05-05 alongside the four-bug-fix
landing in `compositor/qdwin/qdwin.c`. Each scenario double-checks one
qdwin protocol path against a real client.

## Bug coverage map

| Scenario | qdwin behaviour exercised |
|---|---|
| `01-firefox-max-restore.md` | `request_maximize(0)` returns to pre-max geometry (was bug #1) |
| `02-xterm-xwayland-launch.md` | XWayland surface attach doesn't NULL-deref (was bug #2) |
| `03-foot-vs-xterm-tagging.md` | `is_xwayland=1` for X11, `0` for Wayland in `toplevel_added` (was bug #3) |
| `04-cursor-spam-suppressed.md` | `install_default_cursor: no surface yet` logs at most once per session (was bug #4) |
| `05-gtk4-gnome-text-editor.md` | Native Wayland GTK4 toolkit |
| `06-gtk3-thunar-xwayland.md` | XWayland GTK3 toolkit + dbus-activated apps |
| `07-qt5-vlc.md` | Qt5 widget app via XWayland |
| `08-electron-chromium.md` | Electron via XWayland (not native ozone) |
| `09-wxwidgets-audacity.md` | wxWidgets via XWayland |
| `10-tk-fltk-swing.md` | Three "small toolkits" (Tk, FLTK, Java Swing) round-trip |
| `11-imlib2-feh.md` | Raw Xlib + Imlib2 (no toolkit at all) |
| `12-keystroke-roundtrip.md` | wl_keyboard delivery to focused native + XWayland clients |

## Running

The scenarios use a shared helper at `qdwin-apps-helpers.sh`. Source it
once per session:

```bash
source phase1/gui-tests/qdwin-apps/qdwin-apps-helpers.sh
qdwin_apps_set_vm "${VMNAME:-apps-qdwin-...}"
qdwin_apps_session_up || { echo "FAIL: bystander/weston not healthy"; exit 1; }
```

The helper provides:

- `qdwin_apps_launch <name> <cmd>` — start an app as `admin` against the
  active wayland socket with the standard env (`MOZ_ENABLE_WAYLAND=1`,
  `QT_QPA_PLATFORM=wayland`, `GDK_BACKEND=wayland`, `DISPLAY=:0`).
- `qdwin_apps_ctl <command>` — push a line to the bystander FIFO. e.g.
  `qdwin_apps_ctl max`, `qdwin_apps_ctl "max 7"`.
- `qdwin_apps_screenshot <path>` — `virsh screenshot` to host path.
- `qdwin_apps_send_key KEY_*` / `qdwin_apps_type "string"`.
- `qdwin_apps_kill_all` — `pkill -u admin -9` everything we might have
  started; for Cleanup blocks.
- `qdwin_apps_log_grep <pattern>` — pull and grep
  `/home/admin/.local/share/qdwin.log` from the VM.

## As the runner subagent

Each `NN-*.md` is a self-contained scenario. Read it top to bottom,
follow Setup / Steps verbatim, return a PASS/FAIL report referencing
the screenshots. Same pattern as `phase1/gui-tests/qdwin/AGENTS.md`.

**Pitfalls** (read before running):

1. The active wayland socket is whichever weston picked (`wayland-1`
   or `wayland-2`); after a weston crash the names rotate. The helper
   re-detects on every call. Don't hard-code `wayland-1`.
2. `vm-exec` quoting is fragile — wrap multi-line shell in
   `base64 -w0 <<EOF / EOF` and decode in-VM. The helper does this
   automatically; if you write inline `vm-exec "$VM" 'one-liner'`,
   prefer single quotes and avoid embedded `"`.
3. The bystander prints its log to `/tmp/bystander.log`. Tail it before
   asserting — `toplevel_added handle=N` confirms qdwin saw the
   surface even when the screenshot is black.
4. **Cleanup matters.** Each scenario kills its own app in Cleanup;
   stale toplevels obscure the screen for the next scenario. The
   helper's `qdwin_apps_kill_all` is the safety net.
5. Native Wayland Qt apps (kate, qpdfview, qbittorrent) currently
   render black under the pixman renderer. Don't fail the unrelated
   scenarios on that — track via
    instead.

## As the orchestrator

Spawn one Sonnet subagent per scenario, **serially** on a given VM.
The bystander and stale-toplevel pollution mean concurrent scenarios
on one VM produce spurious FAILs. Spin up a second clone via
`compositor/spike-6.5/clone-baseweed.sh apps-qdwin --from-baked` if
you need wall-clock parallelism.

Pre-conditions every runner assumes:

- VM is running, weston is up, bystander is up, bystander FIFO is at
  `/run/user/1000/qdwin-cmd.fifo`.
- `firefox`, `thunderbird`, `xterm`, `xeyes`, `foot`, `gnome-text-editor`,
  `thunar`, `vlc`, `audacity`, `chromium`, `gpick`, `feh`, `eog`,
  `gedit`, `evince`, `inkscape` all installed (one-shot
  `zypper -n install` from the matrix run on 2026-05-05; bake into a
  fresh image when the matrix is stable).
- For the Tk/FLTK/Swing scenarios: `python313-tk`, `libfltk1_3`,
  `fltk-devel`, `java-25-openjdk-devel` installed; demo source files
  live under `phase1/gui-tests/qdwin-apps/demos/` and are served
  from the host HTTP server rooted at the qdistro repo top.

If any precondition is missing, runners should fail with
`INFRA: <thing>` and the orchestrator decides whether to bake/install
or skip.
