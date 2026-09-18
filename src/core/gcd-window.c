/*
 * gcd-window.c — GcdWindow lifecycle + helpers.
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "gcd/gcd-window.h"

GcdWindow *gcd_window_new(const gchar *id)
{
  GcdWindow *w = g_new0(GcdWindow, 1);
  w->id = g_strdup(id);
  w->workspace = -1;
  w->origin = g_strdup("local");
  return w;
}

GcdWindow *gcd_window_copy(const GcdWindow *w)
{
  GcdWindow *c;
  g_return_val_if_fail(w != NULL, NULL);
  c = g_new0(GcdWindow, 1);
  c->id        = g_strdup(w->id);
  c->app_id    = g_strdup(w->app_id);
  c->title     = g_strdup(w->title);
  c->x         = w->x;
  c->y         = w->y;
  c->width     = w->width;
  c->height    = w->height;
  c->workspace = w->workspace;
  c->flags     = w->flags;
  c->origin    = g_strdup(w->origin);
  return c;
}

void gcd_window_free(GcdWindow *w)
{
  if (!w) return;
  g_free(w->id);
  g_free(w->app_id);
  g_free(w->title);
  g_free(w->origin);
  g_free(w);
}

static gboolean str_eq(const gchar *a, const gchar *b)
{
  return g_strcmp0(a, b) == 0;
}

gboolean gcd_window_same_state(const GcdWindow *a, const GcdWindow *b)
{
  g_return_val_if_fail(a && b, FALSE);
  return a->x == b->x && a->y == b->y &&
         a->width == b->width && a->height == b->height &&
         a->workspace == b->workspace && a->flags == b->flags &&
         str_eq(a->app_id, b->app_id) && str_eq(a->title, b->title) &&
         str_eq(a->origin, b->origin);
}

gchar *gcd_window_summary(const GcdWindow *w)
{
  g_return_val_if_fail(w, NULL);
  return g_strdup_printf(
      "%s [%s] \"%s\" app=%s geo=%dx%d+%d+%d ws=%d flags=0x%x",
      w->id ? w->id : "?", w->origin ? w->origin : "?",
      w->title ? w->title : "", w->app_id ? w->app_id : "",
      w->width, w->height, w->x, w->y, w->workspace, w->flags);
}

const gchar *gcd_window_op_name(GcdWindowOp op)
{
  switch (op) {
    case GCD_OP_ACTIVATE:          return "activate";
    case GCD_OP_MINIMIZE:          return "minimize";
    case GCD_OP_UNMINIMIZE:        return "unminimize";
    case GCD_OP_CLOSE:             return "close";
    case GCD_OP_MOVE_TO_WORKSPACE: return "move_to_workspace";
  }
  return "unknown";
}

gboolean gcd_window_op_from_name(const gchar *name, GcdWindowOp *op)
{
  static const struct { const gchar *n; GcdWindowOp o; } table[] = {
    {"activate",          GCD_OP_ACTIVATE},
    {"minimize",          GCD_OP_MINIMIZE},
    {"unminimize",        GCD_OP_UNMINIMIZE},
    {"close",             GCD_OP_CLOSE},
    {"move_to_workspace", GCD_OP_MOVE_TO_WORKSPACE},
  };
  for (guint i = 0; i < G_N_ELEMENTS(table); i++) {
    if (strcmp(table[i].n, name) == 0) {
      *op = table[i].o;
      return TRUE;
    }
  }
  return FALSE;
}
