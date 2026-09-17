# gnome-c-desktop

A GNOME-style desktop environment written in C for Linux. It provides a
cairo-rendered top panel (Activities button, window tasklist, clock) that runs
on **X11** and **Wayland**, plus a window-state **synchronization protocol** so
multiple instances — including shells hosted inside **KWin** — share one merged
list of open windows and can operate on each other's windows **without
reopening applications or rerunning commands**.

```
┌──────────────────────────────────────────┐
│ Activities   [term] [editor]     19:40   │   ← gcd-shell panel
├──────────────────────────────────────────┤
│                                          │
│  local windows        remote windows     │
│  (X11/Wayland)        ([dispB] prefix)   │
└──────────────────────────────────────────┘
```

## Features

- **X11 backend (XCB/EWMH)** — enumerates `_NET_CLIENT_LIST`, tracks
  `_NET_ACTIVE_WINDOW`, geometry, desktops and window flags; issues EWMH
  client messages for activate/minimize/close/move-to-workspace; hosts the
  panel as a `_NET_WM_WINDOW_TYPE_DOCK` window with `_NET_WM_STRUT_PARTIAL`.
- **Wayland backend** — `zwlr_layer_shell_v1` panel surface and
  `zwlr_foreign_toplevel_manager_v1` window tracking/ops (wlroots
  compositors: Sway, river, Hyprland*, labwc…).
- **KWin bridge** — KDE's KWin does not expose wlr-foreign-toplevel, so a
  generated KWin script (`gcd-kwin-bridge.js`, loaded via
  `org.kde.kwin.Scripting`) streams window snapshots to the shell over
  session D-Bus and applies queued window ops. This is what makes the shell
  work on KDE Plasma Wayland.
- **Cross-instance sync (`gcd-syncd` protocol)** — newline-delimited JSON
  over unix sockets or TCP. Instances exchange HELLO/SNAPSHOT/UPSERT/REMOVE
  so every node keeps a *merged* window model tagged by origin. Window ops
  issued against a remote window are forwarded as COMMAND messages and
  applied **on the owning display to the live window** — nothing is relaunched.
- **`gcd-ctl`** — CLI to list the merged model, ping nodes and send window
  ops to any window, local or remote.

## Build

Dependencies:

Debian/Ubuntu (the exact set used by CI):

```sh
sudo apt-get install -y \
  build-essential meson ninja-build pkg-config \
  libglib2.0-dev libxcb1-dev libcairo2-dev \
  libwayland-dev wayland-protocols \
  xvfb            # only for the integration test
```

Fedora: `gcc meson ninja-build glib2-devel libxcb-devel cairo-devel
wayland-devel wayland-protocols-devel`.

Then:

```sh
meson setup build
ninja -C build
meson test -C build
```

Feature switches (`meson_options.txt`): `-Dx11=enabled|disabled`,
`-Dwayland=enabled|disabled`, `-Dtests=true|false`. Both backends are
`auto` by default, so a Wayland-less box still builds the X11 shell.

Install (optional):

```sh
sudo ninja -C build install   # puts gcd-shell/gcd-syncd/gcd-ctl + the KWin script
```

## Run

```sh
# Simplest: auto-detects X11 vs Wayland
./build/gcd-shell

# Explicit backend, named instance (instance names tag windows in sync)
./build/gcd-shell --backend x11 --instance desk-a
```

The panel appears at the top of the screen. Buttons show each window;
focused is highlighted, minimized dimmed. Clicking a window button
activates it. Remote windows (from synced peers) render as
`[peer-name] title`.

### Synchronize two displays

The sync layer is how "the environment running on another display/window"
stays in sync. Start each shell with a unique `--instance` name and link
them with `--peer`:

```sh
# Display A (e.g. :0) — instance 'desk-a'
gcd-shell --instance desk-a --sync-sock /tmp/gcd-a.sock

# Display B (e.g. :10, a nested X server, or another machine over TCP) —
# instance 'desk-b', peering into A
gcd-shell --instance desk-b --sync-sock /tmp/gcd-b.sock \
          --peer unix:/tmp/gcd-a.sock
```

Both panels now list the other display's windows as `[desk-a] …` /
`[desk-b] …`. Clicking a remote window sends a `command` message to the
owning node, which issues the EWMH/Wayland activation **on the live
window** — the application keeps running, untouched.

Check it from the CLI:

```sh
gcd-ctl --sock /tmp/gcd-b.sock list          # merged window list
gcd-ctl --sock /tmp/gcd-b.sock ping          # "pong"
gcd-ctl --sock /tmp/gcd-b.sock activate x11:0x200001
gcd-ctl --sock /tmp/gcd-b.sock minimize x11:0x200001
gcd-ctl --sock /tmp/gcd-b.sock move-ws x11:0x200001 2
```

Headless/relay nodes (e.g. a third machine that just joins the mesh):

```sh
gcd-syncd --backend none --instance relay \
          --tcp-listen 7433 --peer unix:/tmp/gcd-a.sock
```

`gcd-shell`/`gcd-syncd` options (see `--help`): `--peer` is repeatable,
`--sync-sock` picks the unix listen path, `--tcp-listen PORT` accepts TCP
peers, `--connect tcp:HOST:PORT` peers over TCP, `--no-sync` disables
sync, `--no-panel` runs the model+sync only, `--dump-model` prints the
merged model periodically, `--once`/`--once-ms` dump-then-quit.

### Running inside KWin (Plasma Wayland)

KWin hosts no foreign-toplevel protocol, so on Plasma the shell uses the
**KWin bridge** automatically when `--kwin-bridge auto` (default) and no
native toplevel source exists:

1. `gcd-shell` renders `src/kwin/gcd-kwin-bridge.js` into
   `$XDG_RUNTIME_DIR/gnome-c-desktop/kwin-bridge.js` with its unique
   D-Bus service name.
2. Loads it via `org.kde.kwin.Scripting.loadScript` and calls `start`.
3. The script pushes the window list (`Sink`/`SnapshotDone`) and polls
   ops (`Poll`) over session D-Bus — no restart of KWin, no relaunch of
   anything.

Sync then works the same way: another `gcd-shell` (or `gcd-syncd`) on a
different display sees KWin's windows as `[kde-session] …` and can
activate/minimize/close them remotely.

## Tests

```sh
meson test -C build            # all
meson test -C build -v x11-ewmh-smoke   # the integration test
```

- `test-json`, `test-window-model`, `test-sync-proto` — unit tests for the
  JSON DOM, the window model, and the sync wire protocol.
- `x11-ewmh-smoke` — spins up **two** Xvfb displays: display A runs a
  fake EWMH window manager (`tests/fake-wm.c`) with two client windows,
  display B runs a shell peered to A. It asserts B sees A's windows tagged
  `[dispA]` and that `gcd-ctl … activate` on B delivers
  `_NET_ACTIVE_WINDOW` to the WM on A — the full cross-display path.

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — components, threading,
  model flow, KWin bridge design.
- [`docs/SYNC-PROTOCOL.md`](docs/SYNC-PROTOCOL.md) — wire format, message
  reference, handshake semantics.

## License

MIT — see [`LICENSE`](LICENSE).
