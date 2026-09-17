/*
 * gcd-panel.c — GNOME-style top bar.
 *
 * Layout:  [ Activities ] [win][win][win] ... <clock> ... [status]
 * Drawn with cairo's toy text API (no pango dependency). Remote windows
 * (origin != "local") render dimmed with their origin in the label so a
 * user can see which display owns them before clicking.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <time.h>
#include "gcd-panel.h"

typedef struct {
  double x, y, w, h;
  gchar *id;       /* window id, NULL for non-window region */
  enum { HOT_WINDOW, HOT_ACTIVITIES, HOT_STATUS } kind;
} HotRegion;

struct GcdPanel {
  GcdSurface     *surf;
  GcdWindowModel *model;
  GcdPanelOpCb    op_cb;
  gpointer        op_ud;
  GPtrArray      *hot;       /* HotRegion* */
  double          last_x, last_y;
  guint           clock_src;
  gchar          *status;
};

static void hot_clear(GcdPanel *p)
{
  if (p->hot) g_ptr_array_unref(p->hot);
  p->hot = g_ptr_array_new_with_free_func(
      (GDestroyNotify)g_free);
}

static HotRegion *hot_add(GcdPanel *p, double x, double y, double w,
                          double h, int kind, const gchar *id)
{
  HotRegion *r = g_new0(HotRegion, 1);
  r->x = x; r->y = y; r->w = w; r->h = h;
  r->kind = kind;
  r->id = g_strdup(id);
  g_ptr_array_add(p->hot, r);
  return r;
}

static void draw_text(cairo_t *cr, const gchar *text, double x, double y,
                      double max_w)
{
  cairo_text_extents_t ext;
  gchar *shown = g_strdup(text ? text : "");
  /* truncate to fit */
  cairo_text_extents(cr, shown, &ext);
  while (ext.width > max_w && strlen(shown) > 4) {
    shown[strlen(shown) - 4] = '\0';
    strcat(shown, "…");
    cairo_text_extents(cr, shown, &ext);
  }
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, shown);
  g_free(shown);
}

static void paint(GcdPanel *p)
{
  cairo_surface_t *cs = p->surf->get_cairo(p->surf);
  cairo_t *cr = cairo_create(cs);
  double W = p->surf->width, H = p->surf->height;
  double mid = H / 2.0;
  gchar tbuf[64];
  time_t now = time(NULL);
  struct tm tm;
  GList *wins;
  double x;

  /* bar background */
  cairo_set_source_rgb(cr, 0.11, 0.12, 0.13);
  cairo_rectangle(cr, 0, 0, W, H);
  cairo_fill(cr);

  cairo_set_font_size(cr, 12.0);

  hot_clear(p);

  /* Activities */
  cairo_set_source_rgb(cr, 0.20, 0.21, 0.23);
  cairo_rectangle(cr, 4, 3, 86, H - 6);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.92, 0.93, 0.94);
  cairo_move_to(cr, 10, mid + 4);
  cairo_show_text(cr, "Activities");
  hot_add(p, 4, 3, 86, H - 6, HOT_ACTIVITIES, NULL);

  /* task buttons */
  x = 98;
  wins = gcd_window_model_list(p->model);
  for (GList *l = wins; l; l = l->next) {
    const GcdWindow *w = l->data;
    if (w->flags & GCD_WINDOW_SKIP_TASKBAR) continue;

    gchar *label;
    if (g_strcmp0(w->origin, "local") != 0)
      label = g_strdup_printf("[%s] %s", w->origin,
                              w->title ? w->title : w->app_id);
    else
      label = g_strdup(w->title ? w->title :
                       (w->app_id ? w->app_id : "window"));

    double bw = 140;
    gboolean focused = w->flags & GCD_WINDOW_FOCUSED;
    gboolean remote  = g_strcmp0(w->origin, "local") != 0;

    if (focused)
      cairo_set_source_rgb(cr, 0.24, 0.44, 0.68);
    else
      cairo_set_source_rgb(cr, 0.20, 0.21, 0.23);
    cairo_rectangle(cr, x, 3, bw, H - 6);
    cairo_fill(cr);

    if (w->flags & GCD_WINDOW_MINIMIZED)
      cairo_set_source_rgb(cr, 0.55, 0.56, 0.58);
    else if (remote)
      cairo_set_source_rgb(cr, 0.70, 0.80, 0.95);
    else
      cairo_set_source_rgb(cr, 0.92, 0.93, 0.94);
    draw_text(cr, label, x + 8, mid + 4, bw - 14);

    hot_add(p, x, 3, bw, H - 6, HOT_WINDOW, w->id);
    g_free(label);
    x += bw + 4;
    if (x > W * 0.55) break; /* leave room for clock/status */
  }
  g_list_free(wins);

  /* clock, centered (GNOME style) */
  localtime_r(&now, &tm);
  strftime(tbuf, sizeof(tbuf), "%a %b %-d  %H:%M", &tm);
  {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, tbuf, &ext);
    cairo_set_source_rgb(cr, 0.92, 0.93, 0.94);
    cairo_move_to(cr, W / 2 - ext.width / 2, mid + 4);
    cairo_show_text(cr, tbuf);
  }

  /* status, right-aligned */
  if (p->status && *p->status) {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, p->status, &ext);
    cairo_set_source_rgb(cr, 0.70, 0.72, 0.75);
    cairo_move_to(cr, W - ext.width - 10, mid + 4);
    cairo_show_text(cr, p->status);
    hot_add(p, W - ext.width - 16, 3, ext.width + 16, H - 6,
            HOT_STATUS, NULL);
  }

  cairo_destroy(cr);
}

void gcd_panel_repaint(GcdPanel *p)
{
  if (!p || !p->surf) return;
  paint(p);
  p->surf->present(p->surf);
}

static void on_pointer(GcdSurface *s, gdouble x, gdouble y, guint button,
                       gboolean pressed, gpointer ud)
{
  GcdPanel *p = ud;
  (void)s;
  if (x >= 0) p->last_x = x;
  if (y >= 0) p->last_y = y;
  if (!pressed) return;

  for (guint i = 0; i < p->hot->len; i++) {
    HotRegion *r = g_ptr_array_index(p->hot, i);
    if (p->last_x >= r->x && p->last_x < r->x + r->w &&
        p->last_y >= r->y && p->last_y < r->y + r->h) {
      if (r->kind == HOT_WINDOW && r->id && p->op_cb)
        p->op_cb(r->id, GCD_OP_ACTIVATE, 0, p->op_ud);
      else if (r->kind == HOT_ACTIVITIES)
        g_message("panel: Activities clicked (overview not implemented)");
      return;
    }
  }
}

static void on_resize(GcdSurface *s, guint w, guint h, gpointer ud)
{
  GcdPanel *p = ud;
  (void)s;
  p->surf->width = w;
  p->surf->height = h;
  gcd_panel_repaint(p);
}

static gboolean clock_tick(gpointer ud)
{
  gcd_panel_repaint((GcdPanel *)ud);
  return G_SOURCE_CONTINUE;
}

static void model_dirty(GcdWindowModel *m, const GcdWindow *w,
                        gpointer ud)
{
  (void)m; (void)w;
  gcd_panel_repaint((GcdPanel *)ud);
}

static void model_removed(GcdWindowModel *m, const gchar *id, gpointer ud)
{
  (void)m; (void)id;
  gcd_panel_repaint((GcdPanel *)ud);
}

GcdPanel *gcd_panel_new(GcdSurface *surf, GcdWindowModel *merged,
                        GcdPanelOpCb op_cb, gpointer op_ud)
{
  GcdPanel *p = g_new0(GcdPanel, 1);
  GcdWindowModelListener l = {0};

  p->surf = surf;
  p->model = merged;
  p->op_cb = op_cb;
  p->op_ud = op_ud;
  hot_clear(p);

  surf->user_data  = p;
  surf->on_pointer = on_pointer;
  surf->on_resize  = on_resize;

  l.user_data = p;
  l.added = model_dirty;
  l.changed = model_dirty;
  l.removed = model_removed;
  gcd_window_model_add_listener(merged, &l);

  p->clock_src = g_timeout_add_seconds(30, clock_tick, p);
  gcd_panel_repaint(p);
  return p;
}

void gcd_panel_set_status(GcdPanel *p, const gchar *status)
{
  g_free(p->status);
  p->status = g_strdup(status);
  gcd_panel_repaint(p);
}

void gcd_panel_free(GcdPanel *p)
{
  if (!p) return;
  if (p->clock_src) g_source_remove(p->clock_src);
  if (p->hot) g_ptr_array_unref(p->hot);
  if (p->surf && p->surf->destroy) p->surf->destroy(p->surf);
  g_free(p->status);
  g_free(p);
}
