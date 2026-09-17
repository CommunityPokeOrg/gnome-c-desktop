/*
 * gcd-sync-proto.c — wire codec for the sync protocol.
 *
 * Wire format: one compact JSON object per line (LF-terminated). See
 * docs/SYNC-PROTOCOL.md for the schema.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "gcd/gcd-sync.h"
#include "gcd/gcd-json.h"

/* -------------------------------------------------------- window codec */

struct GcdJson *gcd_sync_window_to_json(const GcdWindow *w)
{
  struct GcdJson *j = gcd_json_obj_new();
  gcd_json_obj_set_str(j, "id",        w->id ? w->id : "");
  gcd_json_obj_set_str(j, "app_id",    w->app_id ? w->app_id : "");
  gcd_json_obj_set_str(j, "title",     w->title ? w->title : "");
  gcd_json_obj_set_num(j, "x",         w->x);
  gcd_json_obj_set_num(j, "y",         w->y);
  gcd_json_obj_set_num(j, "width",     w->width);
  gcd_json_obj_set_num(j, "height",    w->height);
  gcd_json_obj_set_num(j, "workspace", w->workspace);
  gcd_json_obj_set_num(j, "flags",     w->flags);
  gcd_json_obj_set_str(j, "origin",    w->origin ? w->origin : "local");
  return j;
}

GcdWindow *gcd_sync_window_from_json(const struct GcdJson *j)
{
  GcdWindow *w;
  const gchar *id;
  if (!j || gcd_json_type(j) != GCD_JSON_OBJ) return NULL;
  id = gcd_json_obj_str(j, "id");
  if (!id || !*id) return NULL;
  w = gcd_window_new(id);
  w->app_id    = g_strdup(gcd_json_obj_str(j, "app_id"));
  w->title     = g_strdup(gcd_json_obj_str(j, "title"));
  w->x         = (gint)gcd_json_obj_num(j, "x", 0);
  w->y         = (gint)gcd_json_obj_num(j, "y", 0);
  w->width     = (gint)gcd_json_obj_num(j, "width", 0);
  w->height    = (gint)gcd_json_obj_num(j, "height", 0);
  w->workspace = (gint)gcd_json_obj_num(j, "workspace", -1);
  w->flags     = (guint)gcd_json_obj_num(j, "flags", 0);
  {
    const gchar *origin = gcd_json_obj_str(j, "origin");
    if (origin) { g_free(w->origin); w->origin = g_strdup(origin); }
  }
  return w;
}

/* ---------------------------------------------------------- message io */

void gcd_sync_msg_init(GcdSyncMsg *msg)
{
  memset(msg, 0, sizeof(*msg));
  msg->type = GCD_SYNC_MSG_INVALID;
  msg->version = GCD_SYNC_PROTOCOL_VERSION;
}

void gcd_sync_msg_clear(GcdSyncMsg *msg)
{
  g_free(msg->instance);
  g_free(msg->id);
  g_free(msg->error);
  if (msg->window)  gcd_window_free(msg->window);
  if (msg->windows) g_ptr_array_unref(msg->windows);
  gcd_sync_msg_init(msg);
}

const gchar *gcd_sync_msg_type_name(GcdSyncMsgType t)
{
  switch (t) {
    case GCD_SYNC_MSG_HELLO:     return "hello";
    case GCD_SYNC_MSG_SNAPSHOT:  return "snapshot";
    case GCD_SYNC_MSG_UPSERT:    return "upsert";
    case GCD_SYNC_MSG_REMOVE:    return "remove";
    case GCD_SYNC_MSG_COMMAND:   return "command";
    case GCD_SYNC_MSG_SUBSCRIBE: return "subscribe";
    case GCD_SYNC_MSG_PING:      return "ping";
    case GCD_SYNC_MSG_PONG:      return "pong";
    case GCD_SYNC_MSG_BYE:       return "bye";
    default:                     return "invalid";
  }
}

static GcdSyncMsgType msg_type_from_name(const gchar *s)
{
  for (int i = GCD_SYNC_MSG_HELLO; i <= GCD_SYNC_MSG_BYE; i++)
    if (g_strcmp0(s, gcd_sync_msg_type_name(i)) == 0)
      return (GcdSyncMsgType)i;
  return GCD_SYNC_MSG_INVALID;
}

gchar *gcd_sync_msg_encode(const GcdSyncMsg *msg)
{
  struct GcdJson *j = gcd_json_obj_new();
  gchar *out;

  gcd_json_obj_set_str(j, "type", gcd_sync_msg_type_name(msg->type));
  if (msg->seq) gcd_json_obj_set_num(j, "seq", msg->seq);

  switch (msg->type) {
    case GCD_SYNC_MSG_HELLO:
      gcd_json_obj_set_num(j, "version", msg->version);
      gcd_json_obj_set_str(j, "instance",
                           msg->instance ? msg->instance : "anonymous");
      break;
    case GCD_SYNC_MSG_SNAPSHOT: {
      struct GcdJson *arr = gcd_json_arr_new();
      if (msg->windows)
        for (guint i = 0; i < msg->windows->len; i++)
          gcd_json_arr_add(arr, gcd_sync_window_to_json(
              g_ptr_array_index(msg->windows, i)));
      gcd_json_obj_set(j, "windows", arr);
      break;
    }
    case GCD_SYNC_MSG_UPSERT:
      gcd_json_obj_set(j, "window", gcd_sync_window_to_json(msg->window));
      break;
    case GCD_SYNC_MSG_REMOVE:
      gcd_json_obj_set_str(j, "id", msg->id ? msg->id : "");
      break;
    case GCD_SYNC_MSG_COMMAND:
      gcd_json_obj_set_str(j, "id", msg->id ? msg->id : "");
      gcd_json_obj_set_str(j, "op", gcd_window_op_name(msg->op));
      gcd_json_obj_set_num(j, "arg", msg->arg);
      break;
    case GCD_SYNC_MSG_SUBSCRIBE:
    case GCD_SYNC_MSG_PING:
    case GCD_SYNC_MSG_PONG:
    case GCD_SYNC_MSG_BYE:
      break;
    default:
      break;
  }

  out = gcd_json_to_string(j);
  gcd_json_free(j);
  {
    gchar *line = g_strconcat(out, "\n", NULL);
    g_free(out);
    return line;
  }
}

gboolean gcd_sync_msg_decode(const gchar *line, GcdSyncMsg *msg,
                             GError **error)
{
  struct GcdJson *j;
  const gchar *type;
  gboolean ok = FALSE;

  gcd_sync_msg_init(msg);
  j = gcd_json_parse(line, -1, error);
  if (!j) return FALSE;
  if (gcd_json_type(j) != GCD_JSON_OBJ) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "sync message is not a JSON object");
    goto out;
  }
  type = gcd_json_obj_str(j, "type");
  msg->type = msg_type_from_name(type);
  if (msg->type == GCD_SYNC_MSG_INVALID) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "unknown sync message type '%s'", type ? type : "(none)");
    goto out;
  }
  msg->seq = (guint)gcd_json_obj_num(j, "seq", 0);

  switch (msg->type) {
    case GCD_SYNC_MSG_HELLO:
      msg->version  = (guint)gcd_json_obj_num(j, "version", 0);
      msg->instance = g_strdup(gcd_json_obj_str(j, "instance"));
      break;
    case GCD_SYNC_MSG_SNAPSHOT: {
      const struct GcdJson *arr = gcd_json_obj_get(j, "windows");
      gsize n = gcd_json_arr_len(arr);
      msg->windows = g_ptr_array_new_with_free_func(
          (GDestroyNotify)gcd_window_free);
      for (gsize i = 0; i < n; i++) {
        GcdWindow *w =
            gcd_sync_window_from_json(gcd_json_arr_get(arr, i));
        if (w) g_ptr_array_add(msg->windows, w);
      }
      break;
    }
    case GCD_SYNC_MSG_UPSERT:
      msg->window = gcd_sync_window_from_json(gcd_json_obj_get(j, "window"));
      if (!msg->window) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "upsert message missing valid 'window'");
        goto out;
      }
      break;
    case GCD_SYNC_MSG_REMOVE:
      msg->id = g_strdup(gcd_json_obj_str(j, "id"));
      break;
    case GCD_SYNC_MSG_COMMAND: {
      const gchar *op = gcd_json_obj_str(j, "op");
      msg->id  = g_strdup(gcd_json_obj_str(j, "id"));
      msg->arg = (gint)gcd_json_obj_num(j, "arg", 0);
      if (!op || !gcd_window_op_from_name(op, &msg->op)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "command message has unknown op '%s'",
                    op ? op : "(none)");
        goto out;
      }
      break;
    }
    case GCD_SYNC_MSG_SUBSCRIBE:
    case GCD_SYNC_MSG_PING:
    case GCD_SYNC_MSG_PONG:
    case GCD_SYNC_MSG_BYE:
      break;
    default:
      break;
  }
  ok = TRUE;
out:
  gcd_json_free(j);
  return ok;
}
