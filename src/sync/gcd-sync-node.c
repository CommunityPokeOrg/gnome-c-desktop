/*
 * gcd-sync-node.c — the synchronization node embedded in gcd-shell and
 * gcd-syncd.
 *
 * Responsibilities:
 *   - accept peer connections (unix listener + optional tcp listener)
 *   - dial outbound peers ("unix:…" / "tcp:host:port")
 *   - handshake (HELLO) then exchange SNAPSHOT of the local model
 *   - relay local model changes to peers; merge peer windows into the
 *     merged model tagged with origin=<peer instance>
 *   - route COMMANDs to the display that owns the window, where they
 *     are applied to the live window — never by reopening it
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <gio/gunixsocketaddress.h>
#include "gcd/gcd-sync.h"
#include "gcd/gcd-json.h"

#define MAX_LINE (256 * 1024)

typedef struct {
  GcdSyncNode        *node;
  GSocketConnection  *conn;
  GDataInputStream   *in;
  GOutputStream      *out;
  gchar              *name;     /* remote instance, from HELLO */
  GCancellable       *cancellable;
  gboolean            helloed;
  guint               next_seq;
} Peer;

struct GcdSyncNode {
  gchar          *instance;
  GcdWindowModel *local;    /* not owned */
  GcdWindowModel *merged;   /* not owned */
  GPtrArray      *peers;    /* Peer* */
  GPtrArray      *listeners;/* GSocketListener* */
  GSocketListener *unix_listener;
  GSocketListener *tcp_listener;
  GcdSyncCommandCb command_cb;
  gpointer        command_ud;
  GcdWindowModelListener local_listener; /* registered on local */
};

/* ------------------------------------------------------------ helpers */

static gboolean peer_send(Peer *p, const GcdSyncMsg *msg)
{
  gchar *line = gcd_sync_msg_encode(msg);
  gboolean ok = g_output_stream_write_all(p->out, line, strlen(line),
                                          NULL, NULL, NULL);
  if (!ok)
    g_debug("sync: send to %s failed", p->name ? p->name : "peer");
  g_free(line);
  return ok;
}

static void peer_send_snapshot(Peer *p, GcdWindowModel *model)
{
  GcdSyncMsg m;
  GList *all;
  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_SNAPSHOT;
  m.windows = g_ptr_array_new_with_free_func(
      (GDestroyNotify)gcd_window_free);
  all = gcd_window_model_list(model);
  for (GList *l = all; l; l = l->next)
    g_ptr_array_add(m.windows, gcd_window_copy(l->data));
  g_list_free(all);
  peer_send(p, &m);
  g_ptr_array_unref(m.windows);
}

static void peer_send_hello(Peer *p)
{
  GcdSyncMsg m;
  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_HELLO;
  m.instance = p->node->instance; /* borrowed — do NOT clear */
  peer_send(p, &m);
  /* no gcd_sync_msg_clear: every field is borrowed */
}

static void broadcast(GcdSyncNode *n, const GcdSyncMsg *msg)
{
  for (guint i = 0; i < n->peers->len; i++)
    peer_send(g_ptr_array_index(n->peers, i), msg);
}

/* --------------------------------------------------- peer lifecycle */

static void peer_drop(GcdSyncNode *n, Peer *p)
{
  guint idx;
  if (!g_ptr_array_find(n->peers, p, &idx))
    return;
  if (p->name)
    gcd_window_model_purge_origin(n->merged, p->name);
  g_message("sync: peer %s disconnected",
            p->name ? p->name : "(unhandshaken)");
  g_cancellable_cancel(p->cancellable);
  g_ptr_array_remove_index_fast(n->peers, idx);
  g_object_unref(p->in);
  g_object_unref(p->out);
  g_object_unref(p->conn);
  g_object_unref(p->cancellable);
  g_free(p->name);
  g_free(p);
}

static void peer_handle_msg(GcdSyncNode *n, Peer *p, const GcdSyncMsg *m)
{
  switch (m->type) {
    case GCD_SYNC_MSG_HELLO:
      g_free(p->name);
      p->name = g_strdup(m->instance ? m->instance : "unknown");
      p->helloed = TRUE;
      if (m->version != GCD_SYNC_PROTOCOL_VERSION)
        g_warning("sync: peer %s speaks protocol v%u (we are v%d)",
                  p->name, m->version, GCD_SYNC_PROTOCOL_VERSION);
      g_message("sync: peer '%s' connected", p->name);
      /* now that we know who they are, give them our state */
      peer_send_snapshot(p, n->local);
      break;

    case GCD_SYNC_MSG_SNAPSHOT:
      if (p->name)
        gcd_window_model_purge_origin(n->merged, p->name);
      for (guint i = 0; i < (m->windows ? m->windows->len : 0); i++) {
        GcdWindow *w = gcd_window_copy(g_ptr_array_index(m->windows, i));
        g_free(w->origin);
        w->origin = g_strdup(p->name ? p->name : "unknown");
        gcd_window_model_upsert(n->merged, w);
        gcd_window_free(w);
      }
      break;

    case GCD_SYNC_MSG_UPSERT:
      if (m->window) {
        GcdWindow *w = gcd_window_copy(m->window);
        g_free(w->origin);
        w->origin = g_strdup(p->name ? p->name : "unknown");
        gcd_window_model_upsert(n->merged, w);
        gcd_window_free(w);
      }
      break;

    case GCD_SYNC_MSG_REMOVE:
      if (m->id) {
        const GcdWindow *w = gcd_window_model_find(n->merged, m->id);
        if (w && (!p->name || g_strcmp0(w->origin, p->name) == 0))
          gcd_window_model_remove(n->merged, m->id);
      }
      break;

    case GCD_SYNC_MSG_COMMAND: {
      const GcdWindow *w;
      if (!m->id) break;
      w = gcd_window_model_find(n->local, m->id);
      if (w && n->command_cb) {
        /* a sync peer asking us to operate on OUR window */
        n->command_cb(m->id, m->op, m->arg, n->command_ud);
      } else if (!p->helloed) {
        /* a one-shot client (gcd-ctl): route it to the owner —
         * local via command_cb or remote via the owning peer */
        g_autoptr(GError) err = NULL;
        if (!gcd_sync_node_request_op(n, m->id, m->op, m->arg, &err))
          g_warning("sync: ctl command failed: %s", err->message);
      } else {
        g_debug("sync: command for unknown local window %s", m->id);
      }
      break;
    }

    case GCD_SYNC_MSG_SUBSCRIBE:
      /* one-shot client (gcd-ctl): send merged view */
      peer_send_snapshot(p, n->merged);
      break;

    case GCD_SYNC_MSG_PING: {
      GcdSyncMsg pong;
      gcd_sync_msg_init(&pong);
      pong.type = GCD_SYNC_MSG_PONG;
      peer_send(p, &pong);
      break;
    }
    case GCD_SYNC_MSG_BYE:
      peer_drop(n, p);
      break;

    default:
      break;
  }
}

static void on_line(GObject *src, GAsyncResult *res, gpointer data)
{
  Peer *p = data;
  GcdSyncNode *n = p->node;
  g_autofree gchar *line = NULL;
  gsize len;

  line = g_data_input_stream_read_line_finish(
      G_DATA_INPUT_STREAM(src), res, &len, NULL);
  if (!line) { /* EOF or error */
    peer_drop(n, p);
    return;
  }
  if (len > 0) {
    GcdSyncMsg m;
    g_autoptr(GError) err = NULL;
    if (gcd_sync_msg_decode(line, &m, &err)) {
      peer_handle_msg(n, p, &m);
      gcd_sync_msg_clear(&m);
    } else {
      g_warning("sync: bad message from %s: %s",
                p->name ? p->name : "peer", err->message);
    }
  }
  /* keep reading unless the peer was dropped inside peer_handle_msg */
  {
    guint idx;
    if (!g_ptr_array_find(n->peers, p, &idx)) return;
    g_data_input_stream_read_line_async(
        p->in, G_PRIORITY_DEFAULT, p->cancellable, on_line, p);
  }
}

static Peer *peer_new(GcdSyncNode *n, GSocketConnection *conn)
{
  Peer *p = g_new0(Peer, 1);
  p->node = n;
  p->conn = g_object_ref(conn);
  p->cancellable = g_cancellable_new();
  p->in = g_data_input_stream_new(
      g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  g_data_input_stream_set_newline_type(p->in,
                                       G_DATA_STREAM_NEWLINE_TYPE_LF);
  p->out = g_object_ref(g_io_stream_get_output_stream(G_IO_STREAM(conn)));
  g_ptr_array_add(n->peers, p);
  peer_send_hello(p);
  g_data_input_stream_read_line_async(p->in, G_PRIORITY_DEFAULT,
                                      p->cancellable, on_line, p);
  return p;
}

static void on_accepted(GObject *src, GAsyncResult *res, gpointer data)
{
  GcdSyncNode *n = data;
  GSocketListener *l = G_SOCKET_LISTENER(src);
  g_autoptr(GError) err = NULL;
  GSocketConnection *conn =
      g_socket_listener_accept_finish(l, res, NULL, &err);
  if (!conn) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("sync: accept failed: %s", err->message);
    return; /* stop accepting on error */
  }
  peer_new(n, conn);
  g_object_unref(conn);
  g_socket_listener_accept_async(l, NULL, on_accepted, n);
}

static void start_accepting(GSocketListener *l, GcdSyncNode *n)
{
  g_socket_listener_accept_async(l, NULL, on_accepted, n);
}

/* ------------------------------------------- local model -> broadcast */

static void local_added(GcdWindowModel *m, const GcdWindow *w,
                        gpointer ud)
{
  GcdSyncNode *n = ud;
  GcdSyncMsg msg;
  (void)m;
  if (g_strcmp0(w->origin, "local") != 0) return;
  gcd_window_model_upsert(n->merged, w); /* keep the merged view complete */
  gcd_sync_msg_init(&msg);
  msg.type = GCD_SYNC_MSG_UPSERT;
  msg.window = (GcdWindow *)w; /* encode is read-only; not owned */
  broadcast(n, &msg);
}

static void local_changed(GcdWindowModel *m, const GcdWindow *w,
                          gpointer ud)
{
  local_added(m, w, ud);
}

static void local_removed(GcdWindowModel *m, const gchar *id, gpointer ud)
{
  GcdSyncNode *n = ud;
  GcdSyncMsg msg;
  (void)m;
  gcd_window_model_remove(n->merged, id);
  gcd_sync_msg_init(&msg);
  msg.type = GCD_SYNC_MSG_REMOVE;
  msg.id = (gchar *)id; /* borrowed */
  broadcast(n, &msg);
}

/* ------------------------------------------------------------- public */

GcdSyncNode *gcd_sync_node_new(const gchar *instance,
                               GcdWindowModel *local,
                               GcdWindowModel *merged)
{
  GcdSyncNode *n = g_new0(GcdSyncNode, 1);
  n->instance = g_strdup(instance ? instance : "unknown");
  n->local  = local;
  n->merged = merged;
  n->peers = g_ptr_array_new();

  /* mirror local into merged so the panel sees one model */
  n->local_listener.user_data = n;
  n->local_listener.added = local_added;
  n->local_listener.changed = local_changed;
  n->local_listener.removed = local_removed;
  gcd_window_model_add_listener(local, &n->local_listener);
  return n;
}

void gcd_sync_node_free(GcdSyncNode *n)
{
  if (!n) return;
  while (n->peers->len)
    peer_drop(n, g_ptr_array_index(n->peers, n->peers->len - 1));
  g_ptr_array_unref(n->peers);
  g_clear_object(&n->unix_listener);
  g_clear_object(&n->tcp_listener);
  g_free(n->instance);
  g_free(n);
}

void gcd_sync_node_set_command_cb(GcdSyncNode *n, GcdSyncCommandCb cb,
                                  gpointer ud)
{
  n->command_cb = cb;
  n->command_ud = ud;
}

gboolean gcd_sync_node_listen_unix(GcdSyncNode *n, const gchar *path,
                                   GError **error)
{
  g_autoptr(GSocketAddress) addr = NULL;
  const gchar *p;

  p = path ? path : gcd_sync_default_socket_path();
  g_autofree gchar *dir = g_path_get_dirname(p);
  if (g_mkdir_with_parents(dir, 0700) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "mkdir %s: %s", dir, g_strerror(errno));
    return FALSE;
  }
  unlink(p); /* stale socket */
  addr = g_unix_socket_address_new(p);
  n->unix_listener = g_socket_listener_new();
  if (!g_socket_listener_add_address(n->unix_listener, addr,
                                     G_SOCKET_TYPE_STREAM,
                                     G_SOCKET_PROTOCOL_DEFAULT,
                                     NULL, NULL, error)) {
    g_prefix_error(error, "sync listen %s: ", p);
    g_clear_object(&n->unix_listener);
    return FALSE;
  }
  start_accepting(n->unix_listener, n);
  g_message("sync: listening on unix:%s", p);
  return TRUE;
}

gboolean gcd_sync_node_listen_tcp(GcdSyncNode *n, guint16 port,
                                  GError **error)
{
  n->tcp_listener = g_socket_listener_new();
  if (!g_socket_listener_add_inet_port(n->tcp_listener, port, NULL, error))
    return FALSE;
  start_accepting(n->tcp_listener, n);
  g_message("sync: listening on tcp:%u", port);
  return TRUE;
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer data)
{
  GcdSyncNode *n = data;
  g_autoptr(GError) err = NULL;
  GSocketConnection *conn =
      g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res,
                                             &err);
  if (!conn) {
    g_warning("sync: peer connect failed: %s", err->message);
    return;
  }
  peer_new(n, conn);
  g_object_unref(conn);
}

gboolean gcd_sync_node_connect(GcdSyncNode *n, const gchar *address,
                               GError **error)
{
  g_autoptr(GSocketClient) client = NULL;
  g_autoptr(GSocketAddress) addr = NULL;
  (void)error;

  if (g_str_has_prefix(address, "unix:")) {
    addr = g_unix_socket_address_new(address + 5);
  } else if (g_str_has_prefix(address, "tcp:")) {
    g_auto(GStrv) hp = g_strsplit(address + 4, ":", 2);
    if (!hp[0] || !hp[1]) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "tcp peer needs host:port, got '%s'", address);
      return FALSE;
    }
    addr = g_inet_socket_address_new_from_string(hp[0],
                                                 (guint16)atoi(hp[1]));
    if (!addr) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "bad tcp address '%s'", address);
      return FALSE;
    }
  } else {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "peer address must be unix:PATH or tcp:HOST:PORT");
    return FALSE;
  }

  client = g_socket_client_new();
  g_socket_client_connect_async(client, addr, NULL, on_connected, n);
  g_message("sync: connecting to %s", address);
  return TRUE;
}

gboolean gcd_sync_node_request_op(GcdSyncNode *n, const gchar *window_id,
                                  GcdWindowOp op, gint arg, GError **error)
{
  const GcdWindow *w = gcd_window_model_find(n->merged, window_id);
  if (!w) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "no such window '%s'", window_id);
    return FALSE;
  }
  if (g_strcmp0(w->origin, "local") == 0) {
    if (!n->command_cb) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "no local command handler");
      return FALSE;
    }
    return n->command_cb(window_id, op, arg, n->command_ud)
        ? TRUE : (g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "local op failed"), FALSE);
  }
  /* remote window: forward to the owning peer */
  for (guint i = 0; i < n->peers->len; i++) {
    Peer *p = g_ptr_array_index(n->peers, i);
    if (p->name && g_strcmp0(p->name, w->origin) == 0) {
      GcdSyncMsg m;
      gboolean ok;
      gcd_sync_msg_init(&m);
      m.type = GCD_SYNC_MSG_COMMAND;
      m.id = (gchar *)window_id;
      m.op = op;
      m.arg = arg;
      ok = peer_send(p, &m);
      if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "send to peer %s failed", p->name);
      return ok;
    }
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
              "peer '%s' that owns %s is not connected", w->origin,
              window_id);
  return FALSE;
}

void gcd_sync_node_broadcast_snapshot(GcdSyncNode *n)
{
  for (guint i = 0; i < n->peers->len; i++)
    peer_send_snapshot(g_ptr_array_index(n->peers, i), n->local);
}

guint gcd_sync_node_n_peers(const GcdSyncNode *n)
{
  return n->peers->len;
}

const gchar *gcd_sync_default_socket_path(void)
{
  static gchar path[PATH_MAX];
  const gchar *runtime = g_get_user_runtime_dir();
  if (!runtime || !*runtime) runtime = "/tmp";
  g_snprintf(path, sizeof(path), "%s/gnome-c-desktop/sync.sock", runtime);
  return path;
}
