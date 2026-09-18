/*
 * gcd-kwin.c — KWin D-Bus bridge implementation.
 *
 * Flow:
 *   1. own the bus name org.gcd.KWinBridge<pid>
 *   2. export /org/gcd/KWinBridge implementing org.gcd.KWinBridge
 *      (methods Sink, SnapshotDone, Poll)
 *   3. render gcd-kwin-bridge.js with our service name and load it into
 *      KWin via org.kde.kwin.Scripting.loadScript + start
 *   4. the script pushes window snapshots (Sink × N + SnapshotDone)
 *      and polls Poll() for queued commands which it applies to the
 *      real KWin window objects
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <unistd.h>
#include "gcd/gcd-kwin.h"
#include "gcd/gcd-sync.h"
#include "gcd/gcd-json.h"
#include "gcd-config.h"

#define KWIN_SERVICE   "org.kde.KWin"
#define KWIN_SCRIPTING "/Scripting"
#define KWIN_SCRIPTING_IFACE "org.kde.kwin.Scripting"
#define BRIDGE_IFACE   "org.gcd.KWinBridge"
#define BRIDGE_OBJPATH "/org/gcd/KWinBridge"
#define WINDOW_ID_PREFIX "kwin:"

struct GcdKwinBridge {
  GcdWindowModel *model;      /* borrowed */
  GDBusConnection *bus;       /* owned */
  gchar           *service;   /* org.gcd.KWinBridge<pid> */
  guint            owner_id;
  guint            reg_id;
  GPtrArray       *pending;   /* GcdWindow* seen since last SnapshotDone */
  GHashTable      *last_ids;  /* set of ids applied at last snapshot */
  GPtrArray       *commands;  /* queued GcdJson* ops for KWin script */
};

/* --------------------------------------------------------- D-Bus iface */

/* The KWin script sends the same window JSON shape as the sync layer,
 * with origin forced to "local" and id prefixed "kwin:". */

static void on_method_call(GDBusConnection *conn, const gchar *sender,
                           const gchar *path, const gchar *iface,
                           const gchar *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer data)
{
  GcdKwinBridge *b = data;
  (void)conn; (void)sender; (void)path; (void)iface;

  if (g_strcmp0(method, "Sink") == 0) {
    const gchar *json = NULL;
    g_variant_get(params, "(&s)", &json);
    {
      GcdJson *j = gcd_json_parse(json, -1, NULL);
      if (j) {
        GcdWindow *w = gcd_sync_window_from_json(j);
        if (w) g_ptr_array_add(b->pending, w);
        gcd_json_free(j);
      }
    }
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (g_strcmp0(method, "SnapshotDone") == 0) {
    /* apply pending as the new truth for kwin-prefixed windows */
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < b->pending->len; i++) {
      GcdWindow *w = g_ptr_array_index(b->pending, i);
      g_hash_table_add(seen, (gpointer)w->id);
      gcd_window_model_upsert(b->model, w);
    }
    /* remove kwin windows that disappeared */
    {
      GList *all = gcd_window_model_list(b->model);
      GPtrArray *dead = g_ptr_array_new();
      for (GList *l = all; l; l = l->next) {
        const GcdWindow *w = l->data;
        if (g_str_has_prefix(w->id, WINDOW_ID_PREFIX) &&
            !g_hash_table_contains(seen, w->id))
          g_ptr_array_add(dead, (gpointer)w->id);
      }
      g_list_free(all);
      for (guint i = 0; i < dead->len; i++)
        gcd_window_model_remove(b->model, g_ptr_array_index(dead, i));
      g_ptr_array_free(dead, TRUE);
    }
    g_hash_table_unref(seen);
    g_ptr_array_set_size(b->pending, 0);
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (g_strcmp0(method, "Poll") == 0) {
    GcdJson *arr = gcd_json_arr_new();
    gchar *out;
    for (guint i = 0; i < b->commands->len; i++)
      gcd_json_arr_add(arr, g_ptr_array_index(b->commands, i));
    g_ptr_array_set_size(b->commands, 0); /* ownership moved */
    out = gcd_json_to_string(arr);
    gcd_json_free(arr);
    g_dbus_method_invocation_return_value(inv,
                                          g_variant_new("(s)", out));
    g_free(out);
  } else {
    g_dbus_method_invocation_return_dbus_error(
        inv, "org.gcd.KWinBridge.UnknownMethod", method);
  }
}

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.gcd.KWinBridge'>"
  "    <method name='Sink'>"
  "      <arg type='s' name='window_json' direction='in'/>"
  "    </method>"
  "    <method name='SnapshotDone'/>"
  "    <method name='Poll'>"
  "      <arg type='s' name='commands_json' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

/* ------------------------------------------------------------- setup */

gboolean gcd_kwin_available(GError **error)
{
  g_autoptr(GDBusConnection) bus =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
  g_autoptr(GVariant) ret = NULL;
  gboolean owned = FALSE;
  if (!bus) return FALSE;
  ret = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner",
      g_variant_new("(s)", KWIN_SERVICE),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
  if (!ret) return FALSE;
  g_variant_get(ret, "(b)", &owned);
  return owned;
}

static gchar *render_script(GcdKwinBridge *b, const gchar *template_path,
                            GError **error)
{
  g_autofree gchar *contents = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *dir = NULL;
  gchar **parts;
  GString *out;

  if (!g_file_get_contents(template_path, &contents, NULL, error))
    return NULL;

  dir = g_strdup_printf("%s/gnome-c-desktop", g_get_user_runtime_dir());
  g_mkdir_with_parents(dir, 0700);
  path = g_strdup_printf("%s/kwin-bridge.js", dir);

  out = g_string_new(NULL);
  parts = g_strsplit(contents, "@GCD_SERVICE@", -1);
  for (guint i = 0; parts[i]; i++) {
    g_string_append(out, parts[i]);
    if (parts[i + 1]) g_string_append(out, b->service);
  }
  g_strfreev(parts);

  contents = g_string_free(out, FALSE);
  out = g_string_new(NULL);
  parts = g_strsplit(contents, "@GCD_OBJPATH@", -1);
  for (guint i = 0; parts[i]; i++) {
    g_string_append(out, parts[i]);
    if (parts[i + 1]) g_string_append(out, BRIDGE_OBJPATH);
  }
  g_strfreev(parts);
  g_free(contents);

  if (!g_file_set_contents(path, out->str, -1, error)) {
    g_string_free(out, TRUE);
    return NULL;
  }
  g_string_free(out, TRUE);
  return g_steal_pointer(&path);
}

static void load_script_done(GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) ret =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  GcdKwinBridge *b = data;
  if (!ret) {
    g_warning("kwin bridge: loadScript failed: %s", err->message);
    return;
  }
  /* start the script */
  g_dbus_connection_call(b->bus, KWIN_SERVICE, KWIN_SCRIPTING,
                         KWIN_SCRIPTING_IFACE, "start",
                         NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                         NULL, NULL, NULL);
  g_message("kwin bridge: script loaded and started");
}

GcdKwinBridge *gcd_kwin_bridge_new(GcdWindowModel *model,
                                   const gchar *script_path,
                                   GError **error)
{
  GcdKwinBridge *b = g_new0(GcdKwinBridge, 1);
  g_autofree gchar *rendered = NULL;
  GDBusNodeInfo *info;

  b->model    = model;
  b->pending  = g_ptr_array_new_with_free_func(
      (GDestroyNotify)gcd_window_free);
  b->commands = g_ptr_array_new();
  b->service  = g_strdup_printf("org.gcd.KWinBridge%d", getpid());

  b->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
  if (!b->bus) { gcd_kwin_bridge_free(b); return NULL; }

  info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
  b->reg_id = g_dbus_connection_register_object(
      b->bus, BRIDGE_OBJPATH,
      g_dbus_node_info_lookup_interface(info, BRIDGE_IFACE),
      &(GDBusInterfaceVTable){ .method_call = on_method_call },
      b, NULL, error);
  g_dbus_node_info_unref(info);
  if (!b->reg_id) { gcd_kwin_bridge_free(b); return NULL; }

  b->owner_id = g_bus_own_name_on_connection(
      b->bus, b->service, G_BUS_NAME_OWNER_FLAGS_NONE,
      NULL, NULL, NULL, NULL);

  rendered = render_script(b, script_path, error);
  if (!rendered) { gcd_kwin_bridge_free(b); return NULL; }

  g_dbus_connection_call(b->bus, KWIN_SERVICE, KWIN_SCRIPTING,
                         KWIN_SCRIPTING_IFACE, "loadScript",
                         g_variant_new("(s)", rendered), NULL,
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                         load_script_done, b);
  g_free(rendered);
  return b;
}

void gcd_kwin_bridge_free(GcdKwinBridge *b)
{
  if (!b) return;
  if (b->bus && b->reg_id)
    g_dbus_connection_unregister_object(b->bus, b->reg_id);
  if (b->bus && b->owner_id)
    g_bus_unown_name(b->owner_id);
  g_clear_object(&b->bus);
  if (b->pending) g_ptr_array_unref(b->pending);
  if (b->commands) {
    for (guint i = 0; i < b->commands->len; i++)
      gcd_json_free(g_ptr_array_index(b->commands, i));
    g_ptr_array_unref(b->commands);
  }
  if (b->last_ids) g_hash_table_unref(b->last_ids);
  g_free(b->service);
  g_free(b);
}

gboolean gcd_kwin_bridge_window_op(GcdKwinBridge *b, const gchar *window_id,
                                   GcdWindowOp op, gint arg, GError **error)
{
  GcdJson *cmd;
  if (!g_str_has_prefix(window_id, WINDOW_ID_PREFIX)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "kwin bridge does not own window %s", window_id);
    return FALSE;
  }
  cmd = gcd_json_obj_new();
  gcd_json_obj_set_str(cmd, "id", window_id + strlen(WINDOW_ID_PREFIX));
  gcd_json_obj_set_str(cmd, "op", gcd_window_op_name(op));
  gcd_json_obj_set_num(cmd, "arg", arg);
  g_ptr_array_add(b->commands, cmd);
  return TRUE;
}

gchar *gcd_kwin_bridge_script_path(void)
{
  const gchar *env = getenv("GCD_KWIN_BRIDGE_SCRIPT");
  if (env && g_file_test(env, G_FILE_TEST_EXISTS))
    return g_strdup(env);
  /* dev tree */
  if (g_file_test("src/kwin/gcd-kwin-bridge.js", G_FILE_TEST_EXISTS))
    return g_strdup("src/kwin/gcd-kwin-bridge.js");
  /* installed location */
  {
    gchar *p = g_strdup(GCD_PREFIX "/share/gnome-c-desktop/gcd-kwin-bridge.js");
    if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
    g_free(p);
  }
  return NULL;
}
