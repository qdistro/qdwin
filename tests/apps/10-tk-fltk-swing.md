# 10 — small-toolkit roundtrip: Tk, FLTK, Java Swing

**Acceptance criterion:** three "small toolkit" demo programs each
launch via XWayland, render their widgets correctly, and survive the
bystander's max/restore round-trip. The demos are
`phase1/gui-tests/qdwin-apps/demos/{tk-demo.py,fltk-demo.cxx,SwingDemo.java}`
— checked in to the repo. Each scenario stages them into the VM via
the host HTTP server (mounted at `compositor/`, with this scenarios
dir reachable at `phase1/gui-tests/qdwin-apps/`).

## Setup

```bash
source phase1/gui-tests/qdwin-apps/qdwin-apps-helpers.sh
qdwin_apps_set_vm "${VMNAME}"
qdwin_apps_session_up || { echo "FAIL: bystander/weston not healthy"; exit 1; }
qdwin_apps_kill_all

# Host HTTP server on :8765 must serve the qdistro repo root so the
# scenarios can wget tracked demo files. The harness downloads each
# demo into the VM on demand.
pgrep -f "http.server 8765" >/dev/null || (
    cd ${QDWIN_REPO} && \
    python3 -m http.server 8765 >/tmp/qdistro-http.log 2>&1 &
)
sleep 1
DEMOS=http://10.0.2.2:8765/phase1/gui-tests/qdwin-apps/demos
```

## Steps

### Step 1 — Tk via Python

```bash
qdwin_apps_launch tk "wget -qO /tmp/tk-demo.py $DEMOS/tk-demo.py && python3 /tmp/tk-demo.py"
sleep 6
qdwin_apps_screenshot /tmp/10-step1-tk.png
qdwin_apps_kill_all
```

**Assert (1.1):** screenshot shows the Tk window: title "Tk on qdwin",
"Tkinter demo" header in bold, an entry field with "type here", and
a "Click me" button. Standard Tk theme (grey background, default
ttk widgets).

### Step 2 — FLTK

In-VM build of `compositor/extra/fltk-demo.cxx`. The bake step is
idempotent.

```bash
"$QDWIN_VM_EXEC" "$VMNAME" "wget -qO /tmp/fltk-demo.cxx $DEMOS/fltk-demo.cxx && \
    g++ -o /tmp/fltk-demo /tmp/fltk-demo.cxx -lfltk" \
    | tail -3

qdwin_apps_launch fltk "/tmp/fltk-demo"
sleep 4
qdwin_apps_screenshot /tmp/10-step2-fltk.png
qdwin_apps_kill_all
```

**Assert (2.1):** screenshot shows the FLTK window: title "FLTK on
qdwin", File / Edit menu bar, "FLTK demo" centred header, "Text:"
label with entry "type here", "Click me" button. Distinctive flat
FLTK widgets.

### Step 3 — Java Swing

```bash
"$QDWIN_VM_EXEC" "$VMNAME" "wget -qO /tmp/SwingDemo.java $DEMOS/SwingDemo.java && \
    cd /tmp && javac SwingDemo.java" \
    | tail -3

qdwin_apps_launch swing "cd /tmp && java SwingDemo"
sleep 10
qdwin_apps_screenshot /tmp/10-step3-swing.png
qdwin_apps_kill_all
```

**Assert (3.1):** screenshot shows the Swing window: title "Swing on
qdwin", File / Edit menu bar, "Swing demo" header, "type here" text
field, "Click me" button. Metal (Java default) look-and-feel.

## Cleanup

```bash
qdwin_apps_kill_all
```

## Pass criteria

- Each of Tk/FLTK/Swing renders a window with the named widgets.
- Bystander log shows `xwayland=1` for each toplevel.

## Known failure modes

- **`python3 -c "import tkinter"` fails** — VM is missing
  `python313-tk`. Install in Setup.
- **`g++` not found** — VM missing `gcc-c++`. The fltk-devel pull
  drags it in; double-check `which g++`.
- **`javac` not found** — VM has only `java-25-openjdk-headless` (no
  JDK). Install `java-25-openjdk-devel`.
