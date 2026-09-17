/*
 * gcd-shell.c — the desktop shell process.
 *
 * Owns: one display backend, the merged window model, a sync node, the
 * panel, and (when needed) the KWin bridge.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <locale.h>
#include "gcd/gcd-backend.h"
#include "gcd/gcd-sync.h"
#include "gcd/gcd-kwin.h"
#include "gcd-config.h"
#include "gcd-panel.h"

typedef struct {
  GcdBackend      *backend;
  GcdWindowModel  *local;
  GcdWindowModel  *merged;
  GcdSyncNode     *node;
  GcdPanel        *panel;
  GcdKwinBridge   *kwin;
  GMainLoop       *loop;
} Shell;

static gboolean shell_window_op(const gchar *id, GcdWindowOp op,
                                gint arg, gpointer ud)
{
  Shell *s = ud;
  const GcdWindow *w;
  g_autoptr(GError) err = NULL;

  w = gcd_window_model_find(s->local, id);
  if (!w) {
    g_warning("op %s: window %s not in local model",
              gcd_window_op_name(op), id);
    return FALSE;
  }
  if (g_str_has_prefix(id, "kwin:")) {
    if (!s->kwin) {
      g_warning("kwin window %s but bridge is not running", id);
      return FALSE;
    }
    return gcd_kwin_bridge_window_op(s->kwin, id, op, arg, &err);
  }
  if (!s->backend->iface->window_op(s->backend, w, op, arg, &err)) {
    g_warning("op %s on %s failed: %s",
              gcd_window_op_name(op), id, err->message);
    return FALSE;
  }
  return TRUE;
}

static void panel_op(const gchar *id, GcdWindowOp op, gint arg, gpointer ud)
{
  Shell *s = ud;
  g_autoptr(GError) err = NULL;
  if (!gcd_sync_node_request_op(s->node, id, op, arg, &err))
    g_warning("window op failed: %s", err->message);
}

static void dump_model(GcdWindowModel *m)
{
  GList *all = gcd_window_model_list(m);
  g_print("# windows: %u\n", gcd_window_model_size(m));
  for (GList *l = all; l; l = l->next) {
    g_autofree gchar *line = gcd_window_summary(l->data);
    g_print("%s\n", line);
  }
  g_list_free(all);
  fflush(stdout);
}

static gboolean once_done(gpointer ud)
{
  Shell *s = ud;
  dump_model(s->merged);
  g_main_loop_quit(s->loop);
  return G_SOURCE_REMOVE;
}

static gboolean dump_tick(gpointer ud)
{
  Shell *s = ud;
  dump_model(s->merged);
  return G_SOURCE_CONTINUE;
}

static gchar *opt_backend   = "auto";
static gchar *opt_instance  = NULL;
static gchar *opt_sock      = NULL;
static gchar **opt_peers    = NULL;
static gint   opt_tcp_port  = 0;
static gboolean opt_no_panel  = FALSE;
static gboolean opt_no_sync   = FALSE;
static gboolean opt_once      = FALSE;
static gboolean opt_dump      = FALSE;
static gint   opt_once_ms   = 1500;
static gchar *opt_kwin      = "auto";

static const GOptionEntry opts[] = {
  { "backend",   'b', 0, G_OPTION_ARG_STRING, &opt_backend,
    "Display backend: auto|x11|wayland", "NAME" },
  { "instance",  'i', 0, G_OPTION_ARG_STRING, &opt_instance,
    "Sync instance name (default <host>:<DISPLAY>)", "NAME" },
  { "sync-sock", 's', 0, G_OPTION_ARG_STRING, &opt_sock,
    "Unix socket path for sync", "PATH" },
  { "peer",      'p', 0, G_OPTION_ARG_STRING_ARRAY, &opt_peers,
    "Connect to peer unix:PATH or tcp:HOST:PORT (repeatable)", "ADDR" },
  { "tcp-listen", 0,  0, G_OPTION_ARG_INT, &opt_tcp_port,
    "Also accept sync peers on TCP port", "PORT" },
  { "no-panel",   0,  0, G_OPTION_ARG_NONE, &opt_no_panel,
    "Headless: no panel surface", NULL },
  { "no-sync",    0,  0, G_OPTION_ARG_NONE, &opt_no_sync,
    "Disable window-state sync", NULL },
  { "dump-model", 0,  0, G_OPTION_ARG_NONE, &opt_dump,
    "Print the merged window model on every change", NULL },
  { "once",       0,  0, G_OPTION_ARG_NONE, &opt_once,
    "Collect state for --once-ms then print the model and exit", NULL },
  { "once-ms",    0,  0, G_OPTION_ARG_INT, &opt_once_ms,
    "Collection window for --once (ms)", "MS" },
  { "kwin-bridge",0,  0, G_OPTION_ARG_STRING, &opt_kwin,
    "KWin D-Bus bridge: auto|always|never (default auto)", "MODE" },
  { NULL }
};

static gchar *default_instance(const gchar *backend_name)
{
  const gchar *host = g_get_host_name();
  const gchar *disp = getenv("WAYLAND_DISPLAY");
  if (!disp) disp = getenv("DISPLAY");
  if (!disp) disp = backend_name;
  return g_strdup_printf("%s:%s", host, disp ? disp : "?");
}

int main(int argc, char **argv)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GOptionContext) oc = NULL;
  g_autofree gchar *instance = NULL;
  Shell s = {0};

  setlocale(LC_ALL, "");
  oc = g_option_context_new("— GNOME-style desktop shell (X11 + Wayland)");
  g_option_context_add_main_entries(oc, opts, NULL);
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("%s\n", err->message);
    return 2;
  }

  s.local  = gcd_window_model_new();
  s.merged = gcd_window_model_new();

  s.backend = gcd_backend_open(opt_backend, &err);
  if (!s.backend) {
    g_printerr("backend open failed: %s\n", err->message);
    return 1;
  }
  g_message("backend '%s' up", s.backend->iface->name);

  /* The backend maintains its own model internally — expose it as our
   * local model by re-feeding it, or simplest: use its model directly. */
  {
    GcdWindowModel *bm = s.backend->iface->model(s.backend);
    gcd_window_model_free(s.local);
    s.local = bm; /* owned by backend; treat as borrowed */
  }

  instance = opt_instance ? g_strdup(opt_instance)
                          : default_instance(s.backend->iface->name);

  s.loop = g_main_loop_new(NULL, FALSE);

  if (!s.backend->iface->attach(s.backend, NULL, &err)) {
    g_printerr("backend attach failed: %s\n", err->message);
    return 1;
  }

  /* KWin bridge: 'always', or 'auto' when the backend cannot enumerate
   * toplevels itself (KWin Wayland) and KWin is on the session bus. */
  {
    gboolean want = g_strcmp0(opt_kwin, "always") == 0;
    if (g_strcmp0(opt_kwin, "auto") == 0 &&
        !gcd_backend_has_toplevel_source(s.backend)) {
      g_autoptr(GError) kerr = NULL;
      want = gcd_kwin_available(&kerr);
      if (!want && kerr)
        g_message("kwin bridge auto-check: %s", kerr->message);
    }
    if (want && g_strcmp0(opt_kwin, "never") != 0) {
      g_autofree gchar *script = gcd_kwin_bridge_script_path();
      if (!script) {
        g_warning("kwin bridge requested but script not found "
                  "(set GCD_KWIN_BRIDGE_SCRIPT)");
      } else {
        s.kwin = gcd_kwin_bridge_new(s.local, script, &err);
        if (!s.kwin)
          g_warning("kwin bridge failed to start: %s", err->message);
      }
    }
  }

  if (!opt_no_sync) {
    s.node = gcd_sync_node_new(instance, s.local, s.merged);
    gcd_sync_node_set_command_cb(s.node, shell_window_op, &s);
    if (!gcd_sync_node_listen_unix(s.node, opt_sock, &err))
      g_warning("sync listen failed: %s", err->message);
    if (opt_tcp_port > 0 &&
        !gcd_sync_node_listen_tcp(s.node, (guint16)opt_tcp_port, &err))
      g_warning("sync tcp listen failed: %s", err->message);
    if (opt_peers)
      for (guint i = 0; opt_peers[i]; i++)
        if (!gcd_sync_node_connect(s.node, opt_peers[i], &err))
          g_warning("peer %s: %s", opt_peers[i], err->message);
  } else {
    /* no sync: merged == local; mirror via a node anyway for code-path
     * uniformity — but with no listeners */
    s.node = gcd_sync_node_new(instance, s.local, s.merged);
    gcd_sync_node_set_command_cb(s.node, shell_window_op, &s);
  }

  /* merge whatever the backend already reported */
  {
    GList *all = gcd_window_model_list(s.local);
    for (GList *l = all; l; l = l->next)
      gcd_window_model_upsert(s.merged, l->data);
    g_list_free(all);
  }

  if (!opt_no_panel && !opt_once) {
    g_autoptr(GError) perr = NULL;
    GcdSurface *surf =
        s.backend->iface->panel_create(s.backend, 28, &perr);
    if (surf) {
      s.panel = gcd_panel_new(surf, s.merged, panel_op, &s);
      if (s.node) {
        g_autofree gchar *st =
            g_strdup_printf("peers: %u", gcd_sync_node_n_peers(s.node));
        gcd_panel_set_status(s.panel, st);
      }
    } else {
      g_warning("panel unavailable: %s", perr->message);
    }
  }

  if (opt_dump)
    g_timeout_add_seconds(2, dump_tick, &s);
  if (opt_once)
    g_timeout_add(opt_once_ms, once_done, &s);

  g_message("gcd-shell %s ready (instance=%s)", GCD_VERSION, instance);
  g_main_loop_run(s.loop);

  if (opt_once) { /* dumped in once_done */ }
  gcd_panel_free(s.panel);
  if (s.kwin) gcd_kwin_bridge_free(s.kwin);
  gcd_sync_node_free(s.node);
  s.backend->iface->destroy(s.backend);
  gcd_window_model_free(s.merged);
  g_main_loop_unref(s.loop);
  return 0;
}
