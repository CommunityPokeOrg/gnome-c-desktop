# Architecture

gnome-c-desktop is a set of composable pieces around one core abstraction:
the **window model**.

```
                 ┌─────────────────┐
   X11 (XCB) ──► │                 │      ┌─────────────────┐
   Wayland    ──►│ GcdWindowModel  ├─────►│ GcdSyncNode     │◄──► peers
   KWin       ──►│   (local)       │      │  unix/TCP, JSON │
   (D-Bus)       └─────────────────┘      └────────┬────────┘
                        ▲                        │ merges
                        │                        ▼
                 ┌─────────────────┐      ┌─────────────────┐
                 │ GcdPanel        │◄─────│ GcdWindowModel  │
                 │ (cairo UI)      │      │   (merged)      │
                 └─────────────────┘      └─────────────────┘
```

## Components

### libgcdcore — `src/core/`

- **`GcdWindow`** (`gcd-window`) — immutable-by-convention value object:
  `id`, `app_id`, `title`, geometry, `workspace` (-1 = all), `flags`
  (focused/minimized/maximized/fullscreen/urgent/skip-taskbar/on-top),
  and `origin` (`"local"` or the remote instance name). Window ids are
  namespaced by backend: `x11:0xNNNNNN`, `ftl:N` (foreign-toplevel),
  `kwin:<uuid-ish>`.
- **`GcdWindowModel`** (`gcd-model`) — a hash-table keyed by window id
  with listener callbacks (`added`/`changed`/`removed`). `upsert` emits
  `changed` only when `gcd_window_same_state` detects a real delta.
  `purge_origin` removes every window belonging to a dead peer.
- **`gcd-json`** — a self-contained JSON DOM (parser + serializer) with
  ordered object keys. No external JSON dependency.

### libgcdbackend — `src/backend/`

`GcdBackend` is a vtable interface (`attach`, `model`, `window_op`,
`panel_create`, `destroy`). `gcd_backend_open("auto"|"x11"|"wayland")`
picks one; `gcd_backend_has_toplevel_source()` reports whether the active
backend natively sees application windows (used to decide if the KWin
bridge is needed).

- **X11** (`gcd-backend-x11.c`) — hand-rolled EWMH: interns ~26 atoms,
  diffs `_NET_CLIENT_LIST`, fetches title/class/geometry/state per
  window, tracks `_NET_ACTIVE_WINDOW`, and selects `PROPERTY_CHANGE` on
  each client + `SUBSTRUCTURE_NOTIFY` on root. Ops are EWMH client
  messages (`_NET_ACTIVE_WINDOW`, `WM_CHANGE_STATE`, `_NET_CLOSE_WINDOW`,
  `_NET_WM_DESKTOP`). The panel is a `DOCK` window with
  `_NET_WM_STRUT_PARTIAL` reserving space.
- **Wayland** (`gcd-backend-wayland.c`) — panel via
  `zwlr_layer_shell_v1` (top layer, exclusive zone); windows via
  `zwlr_foreign_toplevel_manager_v1` (`ftl:N` ids). Window ops use
  `activate`/`set_minimized`/`close`; `move_to_workspace` reports
  `NOT_SUPPORTED` (the protocol has no workspace concept). Compositors
  without those protocols still run the shell — with an empty local model
  — which is exactly where the KWin bridge takes over.

### libgcdsync — `src/sync/`

- **`gcd-sync-proto`** — message codec for the newline-delimited JSON
  wire protocol (`docs/SYNC-PROTOCOL.md`).
- **`GcdSyncNode`** (`gcd-sync-node.c`) — owns a `GSocketListener`
  (unix and/or TCP) plus outbound peer connections, all GIO-async on the
  main context. Each peer connection streams length-unbounded lines via
  `GDataInputStream`. Incoming `HELLO` names the peer; snapshots and
  upserts are stamped with `origin=<peer name>` into the **merged**
  model. The node's own local-model listener re-broadcasts changes for
  windows whose `origin == "local"`. Peers that disconnect get their
  windows purged from the merged model.
  - **Command routing**: ops issued by the UI or by `gcd-ctl` go through
    `gcd_sync_node_request_op`, which looks the window up in the merged
    model — `local` windows dispatch to the local command callback,
    remote windows are forwarded as `COMMAND` to the owning peer, which
    applies them to the live window. Un-handshaken connections (gcd-ctl
    clients) get this routing too; handshaken peers' COMMANDs are applied
    directly to the local model.
- **`gcd-syncd`** — the same node in a headless binary; with
  `--backend none` it's a pure relay/mesh hop.

### KWin bridge — `src/kwin/`

KWin Wayland exposes neither foreign-toplevel nor a friendly scripting
control channel, so the bridge is two halves:

- `gcd-kwin.c` (C side) — owns a unique session-bus name
  `org.gcd.KWinBridge<pid>` with an object `/org/gcd/KWinBridge`
  implementing `Sink(s)`, `SnapshotDone()`, `Poll()->s`. It renders the
  JS template (substituting service + object path), loads it via
  `org.kde.kwin.Scripting`, and `start`s it. Batched `Sink` payloads are
  committed to the local model on `SnapshotDone` (with `kwin:`-prefixed
  ids and origin `local`), and `Poll` returns one queued op per call.
- `gcd-kwin-bridge.js` (KWin side) — serializes `workspace.windowList()`
  into the same field shape, debounces pushes on window add/remove/
  activate (~120 ms), and applies commands pulled every 250 ms
  (`activate`, `minimized`, `closeWindow`, `desktops`/`desktop`).

Result: windows living under KWin behave exactly like EWMH/foreign-
toplevel windows — they sync to peers and receive remote ops — **without
restarting KWin or relaunching apps**.

### gcd-shell — `src/shell/`

- `gcd-panel.c` — a `GcdSurface` (from the backend) + cairo drawing:
  Activities button, per-window task buttons (remote ones prefixed
  `[origin]`), centered clock, status area. Pointer events are hit-tested
  against the regions recorded during the last paint; clicking a window
  button calls `gcd_sync_node_request_op(ACTIVATE)` so remote windows
  activate on their own display.
- `gcd-shell.c` — glues it together: opens the backend, creates the sync
  node (unless `--no-sync`), optionally starts the KWin bridge
  (`--kwin-bridge auto|always|never`; auto = needed && KWin is
  reachable), creates the panel (unless `--no-panel`).

### gcd-ctl — `src/sync/gcd-ctl.c`

One-shot client: `list` (SUBSCRIBE → reads until SNAPSHOT), `ping`
(PING → PONG), and `activate|minimize|unminimize|close|move-ws` (COMMAND).

## Threading & event model

Everything runs on the GLib main context of each process: XCB and
`wl_display` fds are wrapped in `GIOChannel`, sockets are `GSocket*`
async ops. No worker threads, no locks — models are mutated only from
the main loop.

## Failure behavior

- A peer that dies: windows are purged on EOF/error, panel updates live.
- `gcd-ctl` one-shot clients disconnect cleanly; the node treats them as
  unhandshaken peers and skips broadcasting to them.
- Ops for unknown/vanished windows log and no-op (`gcd-ctl` reports
  nothing — fire-and-forget by design; the protocol doc covers adding
  `error` replies).
