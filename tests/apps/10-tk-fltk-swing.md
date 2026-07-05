# 10 — small-toolkit roundtrip: Tk, FLTK, Java Swing

**Acceptance criterion:** three "small toolkit" demo programs each
launch via XWayland, render their widgets correctly, and survive the
bystander's max/restore round-trip. The demos are
`tests/apps/demos/{tk-demo.py,fltk-demo.cxx,SwingDemo.java}` — checked
in to the repo next to this scenario. Each step stages the demo into
the VM via a private, free-port host HTTP server rooted at that
`demos/` directory (so `wget $DEMOS/<file>` fetches the tracked
source).

## Setup

```bash
source ${QDWIN_REPO}/tests/apps/qdwin-apps-helpers.sh
qdwin_apps_set_vm "${VMNAME}"
qdwin_apps_session_up || { echo "FAIL: bystander/weston not healthy"; exit 1; }

# App-deps gate (opt-in): Tk/FLTK/Swing are heavy toolkit deps that are only
# baked into the QDWIN_APP_DEPS golden, not the lean GUI golden. Detect each
# toolkit's build/run prerequisite IN THE VM up front:
#   - Tk    : `python3 -c 'import tkinter'`   (python313-tk)
#   - FLTK  : `g++` + the FLTK dev headers    (fltk-devel, pulls gcc-c++)
#   - Swing : `javac`                          (java-25-openjdk-devel)
# If NONE are installed this is a lean golden -> clean SKIP (never ERROR/FAIL),
# exactly like the chromium/vlc scenarios. Mirrors tests/apps/AGENTS.md:
# "an app-specific package absent because the VM was not built with
#  QDWIN_APP_DEPS=1 -> clean SKIP: <app> not installed; qdwin app deps are
#  opt-in". Each present toolkit still runs; a per-step guard below skips any
# individual toolkit whose dep is missing (partial app-deps image) rather than
# erroring.
HAVE_TK=0;    "$QDWIN_VM_EXEC" "$VMNAME" "python3 -c 'import tkinter' 2>/dev/null" && HAVE_TK=1
HAVE_FLTK=0;  "$QDWIN_VM_EXEC" "$VMNAME" 'command -v g++ >/dev/null 2>&1 && test -e /usr/include/FL/Fl.H' && HAVE_FLTK=1
HAVE_SWING=0; "$QDWIN_VM_EXEC" "$VMNAME" 'command -v javac >/dev/null 2>&1' && HAVE_SWING=1
echo "toolkit deps: tk=$HAVE_TK fltk=$HAVE_FLTK swing=$HAVE_SWING"
if [ "$HAVE_TK" = 0 ] && [ "$HAVE_FLTK" = 0 ] && [ "$HAVE_SWING" = 0 ]; then
    echo "SKIP: no Tk/FLTK/Swing toolkits installed (python313-tk / fltk-devel / java-25-openjdk-devel); qdwin app deps are opt-in (rerun with QDWIN_APP_DEPS=1)"
    exit 0
fi

qdwin_apps_kill_all

# Locate the checked-in demo sources on the host. They live in demos/
# next to this scenario (tests/apps/demos), not under a compatibility shim.
DEMOS_DIR=""
for cand in "${QDWIN_REPO:-}/tests/apps/demos" \
            "$QDWIN_WORKSPACE/qdwin/tests/apps/demos"; do
    [ -f "$cand/tk-demo.py" ] && { DEMOS_DIR="$cand"; break; }
done
[ -n "$DEMOS_DIR" ] || { echo "FAIL: demo sources not found (looked for tests/apps/demos/tk-demo.py)"; exit 1; }

# Serve the demos over a PRIVATE, free-port HTTP server so parallel CI
# runs never collide the way the old fixed :8765 singleton did (see the
# vm-smoke port-collision note: 8765 + /tmp/qdistro-http.log were shared
# host singletons). Python binds port 0 — the kernel assigns a free port
# and we serve from that exact live socket, printing the bound port on
# stdout, so there is no bind-probe-then-close race window. Bind 0.0.0.0
# so the guest reaches us at 10.0.2.2 over SLIRP NAT.
DEMOS_PORT_FILE=$(mktemp)
( cd "$DEMOS_DIR" && exec python3 -c '
import http.server, socketserver, sys
socketserver.TCPServer.allow_reuse_address = False
httpd = socketserver.TCPServer(("0.0.0.0", 0), http.server.SimpleHTTPRequestHandler)
sys.stdout.write(str(httpd.server_address[1]) + "\n"); sys.stdout.flush()
httpd.serve_forever()
' > "$DEMOS_PORT_FILE" 2>/tmp/10-demos-http.log ) &
DEMOS_HTTP_PID=$!
DEMOS_PORT=""
for _ in $(seq 1 50); do
    DEMOS_PORT=$(head -1 "$DEMOS_PORT_FILE" 2>/dev/null | tr -dc '0-9')
    [ -n "$DEMOS_PORT" ] && break
    kill -0 "$DEMOS_HTTP_PID" 2>/dev/null || break   # server died before binding
    sleep 0.2
done
[ -n "$DEMOS_PORT" ] || { echo "FAIL: demo HTTP server failed to bind (see /tmp/10-demos-http.log)"; exit 1; }
DEMOS=http://10.0.2.2:$DEMOS_PORT
echo "serving $DEMOS_DIR at $DEMOS (pid $DEMOS_HTTP_PID)"
```

## Steps

### Step 1 — Tk via Python

```bash
# Re-detect Tk inline so this guard is correct even if $HAVE_TK from Setup did
# not persist into this step's shell.
if ! "$QDWIN_VM_EXEC" "$VMNAME" "python3 -c 'import tkinter' 2>/dev/null"; then
    echo "SKIP step 1 (Tk): python313-tk not installed; qdwin app deps are opt-in"
else
qdwin_apps_launch tk "wget -qO /tmp/tk-demo.py $DEMOS/tk-demo.py && python3 /tmp/tk-demo.py" \
    2>&1 | tee /tmp/10-step1-launch.log
sleep 6
qdwin_apps_screenshot /tmp/10-step1-tk.png
# Preserve the demo's own stdout/stderr: the launch helper only returns
# the vm-exec exit, but the app writes to /tmp/tk.log INSIDE the VM.
# Pull it to the host so a failure (e.g. a Tk font error) is diagnosable
# instead of a silent black screenshot.
"$QDWIN_VM_EXEC" "$VMNAME" "cat /tmp/tk.log" 2>&1 | tee /tmp/10-step1-tk.log
qdwin_apps_kill_all

# ENV PREREQUISITE (not a qdwin bug): if the app log shows
# `failed to allocate font`, the VM template lacks a usable X11 bitmap
# font, so Tk cannot allocate its default font and renders a black/empty
# toplevel with no `toplevel_added`. Report this sub-case as an env
# prerequisite (INFRA), per tests/apps/AGENTS.md — NOT a compositor FAIL.
if grep -q 'failed to allocate font' /tmp/10-step1-tk.log 2>/dev/null; then
    echo "INFRA: Tk font allocation failed in VM template (install xorg-x11-fonts / xorg-x11-fonts-core); Tk sub-case is an env prerequisite, not a qdwin bug"
fi
fi
```

**Assert (1.1):** if `HAVE_TK=1`, screenshot shows the Tk window: title "Tk on qdwin",
"Tkinter demo" header in bold, an entry field with "type here", and
a "Click me" button. Standard Tk theme (grey background, default
ttk widgets).

**Assert (1.2):** if instead `/tmp/10-step1-tk.log` contains
`failed to allocate font`, this step is an `INFRA:` env prerequisite
(missing VM font package), not a qdwin FAIL — see Setup/Known failure
modes.

### Step 2 — FLTK

In-VM build of the fetched `fltk-demo.cxx`. The bake step is
idempotent.

```bash
if ! "$QDWIN_VM_EXEC" "$VMNAME" 'command -v g++ >/dev/null 2>&1 && test -e /usr/include/FL/Fl.H'; then
    echo "SKIP step 2 (FLTK): fltk-devel/g++ not installed; qdwin app deps are opt-in"
else
"$QDWIN_VM_EXEC" "$VMNAME" "wget -qO /tmp/fltk-demo.cxx $DEMOS/fltk-demo.cxx && \
    g++ -o /tmp/fltk-demo /tmp/fltk-demo.cxx -lfltk" \
    2>&1 | tee /tmp/10-step2-build.log | tail -3

qdwin_apps_launch fltk "/tmp/fltk-demo" 2>&1 | tee /tmp/10-step2-launch.log
sleep 4
qdwin_apps_screenshot /tmp/10-step2-fltk.png
"$QDWIN_VM_EXEC" "$VMNAME" "cat /tmp/fltk.log" 2>&1 | tee /tmp/10-step2-fltk.log
qdwin_apps_kill_all
fi
```

**Assert (2.1):** if `HAVE_FLTK=1`, screenshot shows the FLTK window: title "FLTK on
qdwin", File / Edit menu bar, "FLTK demo" centred header, "Text:"
label with entry "type here", "Click me" button. Distinctive flat
FLTK widgets.

### Step 3 — Java Swing

```bash
if ! "$QDWIN_VM_EXEC" "$VMNAME" 'command -v javac >/dev/null 2>&1'; then
    echo "SKIP step 3 (Swing): javac (java-25-openjdk-devel) not installed; qdwin app deps are opt-in"
else
"$QDWIN_VM_EXEC" "$VMNAME" "wget -qO /tmp/SwingDemo.java $DEMOS/SwingDemo.java && \
    cd /tmp && javac SwingDemo.java" \
    2>&1 | tee /tmp/10-step3-build.log | tail -3

qdwin_apps_launch swing "cd /tmp && java SwingDemo" 2>&1 | tee /tmp/10-step3-launch.log
sleep 10
qdwin_apps_screenshot /tmp/10-step3-swing.png
"$QDWIN_VM_EXEC" "$VMNAME" "cat /tmp/swing.log" 2>&1 | tee /tmp/10-step3-swing.log
qdwin_apps_kill_all
fi
```

**Assert (3.1):** if `HAVE_SWING=1`, screenshot shows the Swing window: title "Swing on
qdwin", File / Edit menu bar, "Swing demo" header, "type here" text
field, "Click me" button. Metal (Java default) look-and-feel.

## Cleanup

```bash
qdwin_apps_kill_all
# Stop the private demo HTTP server started in Setup.
[ -n "${DEMOS_HTTP_PID:-}" ] && kill "$DEMOS_HTTP_PID" 2>/dev/null || true
[ -n "${DEMOS_PORT_FILE:-}" ] && rm -f "$DEMOS_PORT_FILE" || true
```

## Pass criteria

- Each INSTALLED toolkit (`HAVE_*=1`) renders a window with the named
  widgets, and the bystander log shows `xwayland=1` for its toplevel.
- A toolkit whose dep is absent is a **clean SKIP**, never a FAIL/ERROR:
  Tk/FLTK/Swing are opt-in `QDWIN_APP_DEPS` packages, so on a lean golden
  the whole scenario short-circuits to `SKIP` in Setup, and on a partial
  app-deps image each missing toolkit's step reports `SKIP step N (...)`.
  Overall status is `PASS` if every installed toolkit passed, `SKIP` if
  none were installed.

## Known failure modes

- **`python3 -c "import tkinter"` fails** — VM is missing `python313-tk`.
  This is an opt-in app dep, so the Setup gate records `HAVE_TK=0` and Step 1
  reports `SKIP step 1 (Tk)` — it is NOT a FAIL. Install `python313-tk`
  (via `QDWIN_APP_DEPS=1`) to exercise it.
- **`_tkinter.TclError: failed to allocate font`** — the VM template
  has no usable X11 bitmap font, so Tk cannot allocate its default
  font and the window is black with no `toplevel_added`. This is an
  env/template prerequisite, not a qdwin compositor bug. Ensure
  `xorg-x11-fonts` (and `xorg-x11-fonts-core`) are installed in the
  template; if absent, report the Tk sub-case as
  `INFRA: Tk font allocation failed` per AGENTS.md rather than FAIL.
- **`g++` / FLTK headers not found** — VM missing `gcc-c++` / `fltk-devel`.
  The Setup gate records `HAVE_FLTK=0` and Step 2 reports `SKIP step 2 (FLTK)`
  — an opt-in app dep, not a FAIL. `fltk-devel` drags in `gcc-c++`.
- **`javac` not found** — VM has only `java-25-openjdk-headless` (no JDK),
  or no Java at all. The Setup gate records `HAVE_SWING=0` and Step 3 reports
  `SKIP step 3 (Swing)` — an opt-in app dep, not a FAIL. Install
  `java-25-openjdk-devel` (via `QDWIN_APP_DEPS=1`) to exercise Swing.
