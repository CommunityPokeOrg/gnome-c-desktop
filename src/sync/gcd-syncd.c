/*
 * gcd-syncd.c — headless sync daemon.
 *
 * Runs the same GcdSyncNode as gcd-shell but without a panel. Use it:
 *   - on a display where you want window sync but no shell chrome
 *   - as a relay/aggregator: --backend none, --peer … per display
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include "gcd/gcd-backend.h"
#include "gcd/gcd-sync.h"
#include "gcd/gcd-kwin.h"
#include "gcd-config.h"

static gchar *opt_backend  = "auto";
static gchar *opt_instance = NULL;
static gchar *opt_sock     = NULL;
static gchar **opt_peers   = NULL;
static gint   opt_tcp_port = 0;
static gboolean opt_dump   = FALSE;
static gchar *opt_kwin     = "auto";

static const GOptionEntry opts[] = {
  { "backend",   'b', 0, G_OPTION_ARG_STRING, &opt_backend,
    "auto|x11|wayland|none", "NAME" },
  { "instance",  'i', 0, G_OPTION_ARG_STRING, &opt_instance,
    "Sync instance name", "NAME" },
  { "sync-sock", 's', 0, G_OPTION_ARG_STRING, &opt_sock,
    "Unix socket path", "PATH" },
  { "peer",      'p', 0, G_OPTION_ARG_STRING_ARRAY, &opt_peers,
    "Connect to peer unix:PATH or tcp:HOST:PORT", "ADDR" },
  { "tcp-listen", 0,  0, G_OPTION_ARG_INT, &opt_tcp_port,
    "Accept sync peers on TCP port", "PORT" },
  { "dump-model", 0,  0, G_OPTION_ARG_NONE, &opt_dump,
    "Print the merged window model every 5s", NULL },
  { "kwin-bridge",0,  0, G_OPTION_ARG_STRING, &opt_kwin,
    "auto|always|never", "MODE" },
  { NULL }
};

typedef struct {
  GcdBackend     *backend;
  GcdWindowModel *local;
  GcdWindowModel *merged;
  GcdSyncNode    *node;
  GcdKwinBridge  *kwin;
} Daemon;

static gboolean daemon_window_op(const gchar *id, GcdWindowOp op,
                                 gint arg, gpointer ud)
{
  Daemon *d = ud;
  const GcdWindow *w = gcd_window_model_find(d->local, id);
  g_autoptr(GError) err = NULL;
  if (!w) return FALSE;
  if (g_str_has_prefix(id, "kwin:"))
    return d->kwin
        ? gcd_kwin_bridge_window_op(d->kwin, id, op, arg, &err)
        : FALSE;
  if (!d->backend) return FALSE;
  return d->backend->iface->window_op(d->backend, w, op, arg, &err);
}

static gboolean dump_tick(gpointer ud)
{
  Daemon *d = ud;
  GList *all = gcd_window_model_list(d->merged);
  g_print("--- merged model (%u windows) ---\n",
          gcd_window_model_size(d->merged));
  for (GList *l = all; l; l = l->next) {
    g_autofree gchar *line = gcd_window_summary(l->data);
    g_print("%s\n", line);
  }
  g_list_free(all);
  fflush(stdout);
  return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GOptionContext) oc;
  g_autofree gchar *instance = NULL;
  Daemon d = {0};

  oc = g_option_context_new("— gnome-c-desktop sync daemon");
  g_option_context_add_main_entries(oc, opts, NULL);
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("%s\n", err->message);
    return 2;
  }

  d.local  = gcd_window_model_new();
  d.merged = gcd_window_model_new();

  if (g_strcmp0(opt_backend, "none") != 0) {
    d.backend = gcd_backend_open(opt_backend, &err);
    if (!d.backend) {
      g_printerr("backend open failed: %s\n", err->message);
      return 1;
    }
    gcd_window_model_free(d.local);
    d.local = d.backend->iface->model(d.backend); /* borrowed */
    if (!d.backend->iface->attach(d.backend, NULL, &err)) {
      g_printerr("backend attach failed: %s\n", err->message);
      return 1;
    }
    if (g_strcmp0(opt_kwin, "never") != 0 &&
        (g_strcmp0(opt_kwin, "always") == 0 ||
         !gcd_backend_has_toplevel_source(d.backend))) {
      if (gcd_kwin_available(NULL)) {
        g_autofree gchar *script = gcd_kwin_bridge_script_path();
        if (script)
          d.kwin = gcd_kwin_bridge_new(d.local, script, NULL);
      }
    }
  }

  instance = opt_instance ? g_strdup(opt_instance)
                          : g_strdup_printf("%s:%s", g_get_host_name(),
                                            opt_backend);
  d.node = gcd_sync_node_new(instance, d.local, d.merged);
  gcd_sync_node_set_command_cb(d.node, daemon_window_op, &d);

  if (!gcd_sync_node_listen_unix(d.node, opt_sock, &err))
    g_warning("sync listen failed: %s", err->message);
  if (opt_tcp_port > 0 &&
      !gcd_sync_node_listen_tcp(d.node, (guint16)opt_tcp_port, &err))
    g_warning("tcp listen failed: %s", err->message);
  if (opt_peers)
    for (guint i = 0; opt_peers[i]; i++)
      if (!gcd_sync_node_connect(d.node, opt_peers[i], &err))
        g_warning("peer %s: %s", opt_peers[i], err->message);

  /* mirror existing local windows */
  {
    GList *all = gcd_window_model_list(d.local);
    for (GList *l = all; l; l = l->next)
      gcd_window_model_upsert(d.merged, l->data);
    g_list_free(all);
  }

  if (opt_dump) g_timeout_add_seconds(5, dump_tick, &d);

  g_message("gcd-syncd %s ready (instance=%s)", GCD_VERSION, instance);
  g_main_loop_run(g_main_loop_new(NULL, FALSE));

  if (d.kwin) gcd_kwin_bridge_free(d.kwin);
  gcd_sync_node_free(d.node);
  if (d.backend) d.backend->iface->destroy(d.backend);
  gcd_window_model_free(d.merged);
  return 0;
}
