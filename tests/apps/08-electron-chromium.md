# 08 — Electron / Chromium browser via XWayland

**Acceptance criterion:** Chromium (representing the Electron browser
class) launches via XWayland, renders a real web page, survives a
maximise/restore cycle. The Wayland-Ozone path stays black under the
pixman renderer (tracked separately as the Krita/Obsidian/Kate
"all-black" cluster); this scenario verifies the XWayland fallback,
which is the load-bearing path for most Electron apps in practice.

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
qdwin_apps_launch chromium "chromium --no-sandbox --user-data-dir=/tmp/chr-scenario08"
sleep 18
qdwin_apps_screenshot /tmp/08-step1-launched.png
```

**Assert (1.1):** screenshot shows Chromium with a tab bar (at least
one tab — the openSUSE search default page or `chrome://newtab`),
URL bar visible, and rendered web content (text, links, search box —
not just a blank canvas).
**Assert (1.2):** bystander log shows `toplevel_added handle=<N>
... title="..Chromium" xwayland=1`. (chromium does have a wayland
ozone path, but our test launcher doesn't activate it.)
**Assert (1.3):** weston pid unchanged.

### Step 2 — maximise

```bash
qdwin_apps_ctl "max"
sleep 2
qdwin_apps_screenshot /tmp/08-step2-max.png
```

**Assert (2.1):** chromium fills the screen.

### Step 3 — restore

```bash
qdwin_apps_ctl "restore"
sleep 2
qdwin_apps_screenshot /tmp/08-step3-restore.png
```

**Assert (3.1):** chromium back at launch size.

## Cleanup

```bash
qdwin_apps_kill_all
```

## Pass criteria

- All three screenshots show rendered Chromium (not blank).
- bystander tags chromium `xwayland=1`.

## Known failure modes

- **GPU process exits** — `/tmp/chromium.log` shows
  `Exiting GPU process due to errors during initialization` and the
  canvas stays black. That's the native Wayland Ozone path failing
  under pixman; tracked in
  . Forcing
  `--ozone-platform=wayland` in the launch command would reproduce
  the bug; this scenario deliberately omits it.
- **Test pollution** — leftover `/tmp/chr-scenario08` from a prior
  failed run forces chromium into "Restoring tabs?" UI which doesn't
  match the assertions. Setup should `rm -rf /tmp/chr-scenario08`
  before launch.
