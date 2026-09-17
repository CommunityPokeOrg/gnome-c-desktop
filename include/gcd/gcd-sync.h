/*
 * gcd-sync.h — cross-display / cross-KWin window-state synchronization.
 *
 * Wire protocol: newline-delimited JSON on a unix socket (default
 * $XDG_RUNTIME_DIR/gnome-c-desktop/sync.sock) or TCP for cross-host
 * peers. See docs/SYNC-PROTOCOL.md.
 *
 * Two pieces:
 *   - codec:   gcd_sync_msg_*  — serialize/parse one message
 *   - node:    GcdSyncNode     — listener + peers, mirrors the local
 *              model outward and remote peers' models into the merged
 *              model, and routes window commands to the owning display
 *              (activating a remote window never reopens its app).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <gio/gio.h>
#include "gcd-model.h"
#include "gcd-window.h"

G_BEGIN_DECLS

/* ---------------------------------------------------------------- codec */

#define GCD_SYNC_PROTOCOL_VERSION 1

typedef enum {
  GCD_SYNC_MSG_INVALID = 0,
  GCD_SYNC_MSG_HELLO,       /* handshake: {type,version,instance,seq?} */
  GCD_SYNC_MSG_SNAPSHOT,    /* full window list, replaces peer state */
  GCD_SYNC_MSG_UPSERT,      /* one window added/changed            */
  GCD_SYNC_MSG_REMOVE,      /* one window removed                  */
  GCD_SYNC_MSG_COMMAND,     /* window op to execute on the owner   */
  GCD_SYNC_MSG_SUBSCRIBE,   /* client asks for a snapshot (ctl)    */
  GCD_SYNC_MSG_PING,
  GCD_SYNC_MSG_PONG,
  GCD_SYNC_MSG_BYE,
} GcdSyncMsgType;

typedef struct {
  GcdSyncMsgType type;
  gchar    *instance;    /* HELLO: peer's instance name */
  guint     version;     /* HELLO */
  GPtrArray *windows;    /* SNAPSHOT: GcdWindow* (owned) */
  GcdWindow *window;     /* UPSERT (owned) */
  gchar    *id;          /* REMOVE / COMMAND target */
  GcdWindowOp op;        /* COMMAND */
  gint      arg;         /* COMMAND arg (e.g. workspace) */
  guint     seq;         /* optional sequence number */
  gchar    *error;       /* set when type == INVALID */
} GcdSyncMsg;

void       gcd_sync_msg_init    (GcdSyncMsg *msg);
void       gcd_sync_msg_clear   (GcdSyncMsg *msg);

/* Serialize one message as a newline-terminated JSON string. */
gchar     *gcd_sync_msg_encode  (const GcdSyncMsg *msg);

/* Parse one JSON line (with or without trailing newline). */
gboolean   gcd_sync_msg_decode  (const gchar *line, GcdSyncMsg *msg,
                                 GError **error);

/* window <-> json helpers shared by message codec and dumps */
struct GcdJson;
struct GcdJson *gcd_sync_window_to_json   (const GcdWindow *window);
GcdWindow      *gcd_sync_window_from_json (const struct GcdJson *json);

const gchar *gcd_sync_msg_type_name (GcdSyncMsgType type);

/* ----------------------------------------------------------------- node */

typedef struct GcdSyncNode GcdSyncNode;

/* Executed when a peer asks this display to operate on one of ITS OWN
 * (local, origin=="local") windows. Return value is sent nowhere; log. */
typedef gboolean (*GcdSyncCommandCb)(const gchar *window_id,
                                     GcdWindowOp  op,
                                     gint         arg,
                                     gpointer     user_data);

/* `local`  — model fed by the display backend on this display.
 * `merged` — model rendered by the panel; the node keeps it equal to
 *            local ∪ (all remote peers' windows, origin = peer name).
 * `instance` — unique name for this display/session
 *            (e.g. "host:1" or "host/wayland-0"). */
GcdSyncNode *gcd_sync_node_new  (const gchar    *instance,
                                 GcdWindowModel *local,
                                 GcdWindowModel *merged);
void         gcd_sync_node_free (GcdSyncNode    *node);

void         gcd_sync_node_set_command_cb (GcdSyncNode      *node,
                                           GcdSyncCommandCb  cb,
                                           gpointer          user_data);

/* Listeners. unix path: create dirs as needed; NULL path → default. */
gboolean gcd_sync_node_listen_unix (GcdSyncNode *node, const gchar *path,
                                    GError **error);
gboolean gcd_sync_node_listen_tcp  (GcdSyncNode *node, guint16 port,
                                    GError **error);

/* Outbound peer: "unix:/path/to.sock" or "tcp:host:port". */
gboolean gcd_sync_node_connect     (GcdSyncNode *node, const gchar *address,
                                    GError **error);

/* Send a window command toward the display that owns `window_id`
 * (looks up the merged model; local targets go to the command cb). */
gboolean gcd_sync_node_request_op  (GcdSyncNode *node,
                                    const gchar *window_id,
                                    GcdWindowOp  op, gint arg,
                                    GError **error);

/* Broadcast a snapshot of the local model to every peer. */
void     gcd_sync_node_broadcast_snapshot (GcdSyncNode *node);

guint    gcd_sync_node_n_peers     (const GcdSyncNode *node);

/* Default socket path for this session. Returns a borrowed string from
 * a static buffer — copy if needed. */
const gchar *gcd_sync_default_socket_path (void);

G_END_DECLS
