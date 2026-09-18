# GCD Sync Protocol v1

Newline-delimited JSON. Each message is a single JSON object followed by
`\n`. Transports: unix stream sockets (default
`$XDG_RUNTIME_DIR/gnome-c-desktop/sync.sock`) or TCP
(`--tcp-listen` / `--peer tcp:host:port`).

Every message has `"type"` and `"version"` (currently `1`).

## Handshake

1. Client connects, sends `hello {instance, version}`.
2. Server replies `hello` then `snapshot` of its **local** windows.
3. Client sends its own `snapshot` of local windows.
4. Steady state: `upsert`/`remove` deltas flow in both directions.

A connection that never sends `hello` is a *one-shot client* (gcd-ctl):
it may send `subscribe`, `ping`, and `command`, and is otherwise ignored.

## Messages

| type | direction | fields | meaning |
|------|-----------|--------|---------|
| `hello` | both | `instance` (string), `version` (int) | name this endpoint; required first message from a real peer |
| `snapshot` | both | `windows` (array of window) | full local window set; replaces all windows from that origin |
| `upsert` | both | `window` (object) | create/update one window |
| `remove` | both | `id` (string) | window closed |
| `subscribe` | ctl→node | — | reply: `snapshot` of the **merged** model (all origins) |
| `command` | both | `id`, `op`, `arg` (int) | operate on a window — routed to the owning instance |
| `ping` / `pong` | both | — | liveness |
| `bye` | both | — | graceful disconnect (same as EOF) |
| `error` | (reserved) | `error` (string) | parse/protocol errors; v1 nodes close instead |

## Window object

```json
{
  "id": "x11:0x200001",
  "app_id": "FakeEdit",
  "title": "Fake Editor",
  "x": 0, "y": 0, "width": 0, "height": 0,
  "workspace": -1,
  "flags": 1,
  "origin": "local"
}
```

- `id` is namespaced: `x11:` (XCB window), `ftl:` (foreign-toplevel),
  `kwin:` (KWin bridge). Ids are globally meaningful — a remote copy of
  `x11:0x200001` still identifies the same window on its home display.
- `flags` bitmask: 1 focused, 2 minimized, 4 maximized, 8 fullscreen,
  16 urgent, 32 skip-taskbar, 64 on-top.
- `workspace`: -1 = shown on all workspaces.
- `origin` is **rewritten on ingest**: a node stamps incoming windows
  with the sender's `hello.instance`, so `origin` always equals the
  owning node's name on every other node.

## Ops

`op` values (see `GcdWindowOp`): `activate`, `minimize`, `unminimize`,
`close`, `move_to_workspace` (`arg` = workspace index).

Routing rule: a node looks the window up in its merged model.
`origin == "local"` → apply locally via the backend. Any other origin →
forward the `command` to the peer with that instance name. One-shot
clients get the same routing, so `gcd-ctl` on node B can activate a
window living on display A — delivered as an EWMH/Wayland op against the
**live** window. Nothing is relaunched.

## Consistency model

- Per-origin authority: only the owner mutates a window; other nodes
  hold read-only replicas tagged by origin.
- On disconnect the node purges all windows with `origin` = the lost
  peer's instance.
- Snapshots are authoritative per origin (they purge-and-replace that
  origin's rows); upserts are deltas.
- No seq/ack in v1 — TCP-order delivery keeps streams consistent.
