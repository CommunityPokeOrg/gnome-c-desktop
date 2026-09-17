/*
 * gcd-backend-x11.c — X11 backend: XCB + EWMH/ICCCM.
 *
 * Works against any EWMH-compliant window manager (tested design: KWin
 * on X11, but Openbox/Mutter/etc. behave the same). It tracks
 * _NET_CLIENT_LIST + per-window properties on the root and client
 * windows, and issues window operations as EWMH/ICCCM client messages —
 * the live window changes state; nothing is reopened.
 *
 * The panel is a _NET_WM_WINDOW_TYPE_DOCK window reserving space via
 * _NET_WM_STRUT_PARTIAL, drawn with cairo-xcb.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <xcb/xcb.h>
#include <cairo-xcb.h>
#include <gio/gio.h>

#include "gcd/gcd-backend.h"

#define WINDOW_ID_PREFIX "x11:"
/* atom accessor: b->atoms.X wherever an X11Backend *b is in scope */
#define A(x) (b->atoms.x)

typedef struct {
  xcb_atom_t _NET_CLIENT_LIST;
  xcb_atom_t _NET_ACTIVE_WINDOW;
  xcb_atom_t _NET_CURRENT_DESKTOP;
  xcb_atom_t _NET_NUMBER_OF_DESKTOPS;
  xcb_atom_t _NET_WM_NAME;
  xcb_atom_t _NET_WM_DESKTOP;
  xcb_atom_t _NET_WM_STATE;
  xcb_atom_t _NET_WM_STATE_HIDDEN;
  xcb_atom_t _NET_WM_STATE_MAXIMIZED_VERT;
  xcb_atom_t _NET_WM_STATE_MAXIMIZED_HORZ;
  xcb_atom_t _NET_WM_STATE_FULLSCREEN;
  xcb_atom_t _NET_WM_STATE_DEMANDS_ATTENTION;
  xcb_atom_t _NET_WM_STATE_ABOVE;
  xcb_atom_t _NET_WM_STATE_SKIP_TASKBAR;
  xcb_atom_t _NET_WM_WINDOW_TYPE;
  xcb_atom_t _NET_WM_WINDOW_TYPE_NORMAL;
  xcb_atom_t _NET_WM_WINDOW_TYPE_DIALOG;
  xcb_atom_t _NET_WM_WINDOW_TYPE_DOCK;
  xcb_atom_t _NET_WM_WINDOW_TYPE_DESKTOP;
  xcb_atom_t _NET_WM_STRUT;
  xcb_atom_t _NET_WM_STRUT_PARTIAL;
  xcb_atom_t _NET_CLOSE_WINDOW;
  xcb_atom_t WM_CHANGE_STATE;
  xcb_atom_t UTF8_STRING;
  xcb_atom_t WM_NAME;
  xcb_atom_t WM_CLASS;
} X11Atoms;

typedef struct {
  GcdBackend      base;
  xcb_connection_t *conn;
  xcb_screen_t    *screen;
  int              screen_no;
  X11Atoms         atoms;
  GcdWindowModel  *model;
  GIOChannel      *channel;
  guint            watch_id;
  xcb_window_t     panel_xid;   /* 0 until created; never listed */
  xcb_window_t     active_win;  /* XCB_NONE when none */
  GMainContext    *ctx;
} X11Backend;

typedef struct {
  xcb_window_t  xid;
  xcb_visualtype_t *visual;
  cairo_surface_t  *cs;
  gboolean         mapped;
} X11SurfacePriv;

/* --------------------------------------------------------- atom intern */

static void intern_atoms(X11Backend *b)
{
  static const char *names[] = {
    "_NET_CLIENT_LIST", "_NET_ACTIVE_WINDOW", "_NET_CURRENT_DESKTOP",
    "_NET_NUMBER_OF_DESKTOPS", "_NET_WM_NAME", "_NET_WM_DESKTOP",
    "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_DEMANDS_ATTENTION", "_NET_WM_STATE_ABOVE",
    "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DIALOG",
    "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_STRUT", "_NET_WM_STRUT_PARTIAL", "_NET_CLOSE_WINDOW",
    "WM_CHANGE_STATE", "UTF8_STRING", "WM_NAME", "WM_CLASS",
  };
  enum { N = 26 };
  xcb_atom_t *slots[] = {
    &b->atoms._NET_CLIENT_LIST, &b->atoms._NET_ACTIVE_WINDOW,
    &b->atoms._NET_CURRENT_DESKTOP, &b->atoms._NET_NUMBER_OF_DESKTOPS,
    &b->atoms._NET_WM_NAME, &b->atoms._NET_WM_DESKTOP,
    &b->atoms._NET_WM_STATE, &b->atoms._NET_WM_STATE_HIDDEN,
    &b->atoms._NET_WM_STATE_MAXIMIZED_VERT,
    &b->atoms._NET_WM_STATE_MAXIMIZED_HORZ,
    &b->atoms._NET_WM_STATE_FULLSCREEN,
    &b->atoms._NET_WM_STATE_DEMANDS_ATTENTION,
    &b->atoms._NET_WM_STATE_ABOVE, &b->atoms._NET_WM_STATE_SKIP_TASKBAR,
    &b->atoms._NET_WM_WINDOW_TYPE, &b->atoms._NET_WM_WINDOW_TYPE_NORMAL,
    &b->atoms._NET_WM_WINDOW_TYPE_DIALOG, &b->atoms._NET_WM_WINDOW_TYPE_DOCK,
    &b->atoms._NET_WM_WINDOW_TYPE_DESKTOP, &b->atoms._NET_WM_STRUT,
    &b->atoms._NET_WM_STRUT_PARTIAL, &b->atoms._NET_CLOSE_WINDOW,
    &b->atoms.WM_CHANGE_STATE, &b->atoms.UTF8_STRING,
    &b->atoms.WM_NAME, &b->atoms.WM_CLASS,
  };
  xcb_intern_atom_cookie_t cookies[N];
  for (guint i = 0; i < G_N_ELEMENTS(names); i++)
    cookies[i] = xcb_intern_atom(b->conn, 0, strlen(names[i]), names[i]);
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(b->conn, cookies[i], NULL);
    *slots[i] = r ? r->atom : XCB_ATOM_NONE;
    free(r);
  }
}

/* --------------------------------------------------------- prop helpers */

static xcb_get_property_reply_t *get_prop(X11Backend *b, xcb_window_t win,
                                          xcb_atom_t prop, xcb_atom_t type)
{
  xcb_get_property_cookie_t c =
      xcb_get_property(b->conn, 0, win, prop, type, 0, 1024);
  return xcb_get_property_reply(b->conn, c, NULL);
}

static gchar *prop_text(X11Backend *b, xcb_window_t win)
{
  xcb_get_property_reply_t *r;
  gchar *out = NULL;
  r = get_prop(b, win, A(_NET_WM_NAME), A(UTF8_STRING));
  if (r && xcb_get_property_value_length(r) > 0)
    out = g_strndup(xcb_get_property_value(r),
                    xcb_get_property_value_length(r));
  free(r);
  if (!out) {
    r = get_prop(b, win, A(WM_NAME), XCB_ATOM_STRING);
    if (r && xcb_get_property_value_length(r) > 0)
      out = g_strndup(xcb_get_property_value(r),
                      xcb_get_property_value_length(r));
    free(r);
  }
  return out;
}

static gchar *prop_class(X11Backend *b, xcb_window_t win)
{
  /* WM_CLASS = "instance\0class\0" — we take the class half. */
  xcb_get_property_reply_t *r = get_prop(b, win, A(WM_CLASS), XCB_ATOM_STRING);
  gchar *out = NULL;
  if (r) {
    const char *p = xcb_get_property_value(r);
    int n = xcb_get_property_value_length(r);
    const char *second = memchr(p, '\0', n);
    if (second && second + 1 < p + n && second[1] != '\0')
      out = g_strndup(second + 1, n - (int)(second + 1 - p));
    else if (n > 0)
      out = g_strndup(p, n);
  }
  free(r);
  return out;
}

static gint prop_cardinal(X11Backend *b, xcb_window_t win, xcb_atom_t prop,
                          gint fallback)
{
  gint out = fallback;
  xcb_get_property_reply_t *r = get_prop(b, win, prop, XCB_ATOM_CARDINAL);
  if (r && xcb_get_property_value_length(r) >= 4)
    out = (gint) * (uint32_t *) xcb_get_property_value(r);
  free(r);
  return out;
}

/* -------------------------------------------------------- window fetch */

static gboolean window_type_listable(X11Backend *b, xcb_window_t win)
{
  xcb_get_property_reply_t *r =
      get_prop(b, win, A(_NET_WM_WINDOW_TYPE), XCB_ATOM_ATOM);
  gboolean ok = TRUE;
  if (r && xcb_get_property_value_length(r) >= 4) {
    xcb_atom_t t = *(xcb_atom_t *) xcb_get_property_value(r);
    if (t == A(_NET_WM_WINDOW_TYPE_DESKTOP) || t == A(_NET_WM_WINDOW_TYPE_DOCK))
      ok = FALSE;
  }
  free(r);
  return ok;
}

static void watch_window(X11Backend *b, xcb_window_t win)
{
  uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                  XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_change_window_attributes(b->conn, win, XCB_CW_EVENT_MASK, &mask);
}

static void fetch_window(X11Backend *b, xcb_window_t win)
{
  if (win == b->panel_xid || !window_type_listable(b, win))
    return;

  g_autofree gchar *id = g_strdup_printf(WINDOW_ID_PREFIX "0x%x", win);
  GcdWindow *w = gcd_window_new(id);

  xcb_get_geometry_reply_t *geo =
      xcb_get_geometry_reply(b->conn, xcb_get_geometry(b->conn, win), NULL);
  if (geo) {
    xcb_translate_coordinates_reply_t *tr = xcb_translate_coordinates_reply(
        b->conn,
        xcb_translate_coordinates(b->conn, win, b->screen->root, 0, 0),
        NULL);
    w->x = tr ? tr->dst_x : geo->x;
    w->y = tr ? tr->dst_y : geo->y;
    w->width  = geo->width;
    w->height = geo->height;
    free(tr);
    free(geo);
  }

  w->title  = prop_text(b, win);
  w->app_id = prop_class(b, win);
  w->workspace = prop_cardinal(b, win, A(_NET_WM_DESKTOP), -1);

  guint flags = 0;
  xcb_get_property_reply_t *st =
      get_prop(b, win, A(_NET_WM_STATE), XCB_ATOM_ATOM);
  if (st) {
    int n = xcb_get_property_value_length(st) / (int)sizeof(xcb_atom_t);
    xcb_atom_t *list = xcb_get_property_value(st);
    for (int i = 0; i < n; i++) {
      xcb_atom_t s = list[i];
      if (s == A(_NET_WM_STATE_HIDDEN))             flags |= GCD_WINDOW_MINIMIZED;
      else if (s == A(_NET_WM_STATE_MAXIMIZED_VERT) ||
               s == A(_NET_WM_STATE_MAXIMIZED_HORZ)) flags |= GCD_WINDOW_MAXIMIZED;
      else if (s == A(_NET_WM_STATE_FULLSCREEN))     flags |= GCD_WINDOW_FULLSCREEN;
      else if (s == A(_NET_WM_STATE_DEMANDS_ATTENTION)) flags |= GCD_WINDOW_URGENT;
      else if (s == A(_NET_WM_STATE_ABOVE))          flags |= GCD_WINDOW_ON_TOP;
      else if (s == A(_NET_WM_STATE_SKIP_TASKBAR))   flags |= GCD_WINDOW_SKIP_TASKBAR;
    }
    free(st);
  }
  if (win == b->active_win) flags |= GCD_WINDOW_FOCUSED;
  w->flags = flags;

  gcd_window_model_upsert(b->model, w);
  gcd_window_free(w);
}

static void refresh_client_list(X11Backend *b)
{
  xcb_get_property_reply_t *r =
      get_prop(b, b->screen->root, A(_NET_CLIENT_LIST), XCB_ATOM_WINDOW);
  GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
  GPtrArray *dead = g_ptr_array_new();

  if (r) {
    int n = xcb_get_property_value_length(r) / (int)sizeof(xcb_window_t);
    xcb_window_t *list = xcb_get_property_value(r);
    for (int i = 0; i < n; i++) {
      xcb_window_t win = list[i];
      if (win == b->panel_xid) continue;
      g_hash_table_add(seen, GUINT_TO_POINTER(win));
      g_autofree gchar *id = g_strdup_printf(WINDOW_ID_PREFIX "0x%x", win);
      if (!gcd_window_model_find(b->model, id)) {
        watch_window(b, win);
        fetch_window(b, win);
      }
    }
    free(r);
  }

  /* drop windows that vanished from the client list */
  GList *all = gcd_window_model_list(b->model);
  for (GList *l = all; l; l = l->next) {
    const GcdWindow *w = l->data;
    if (g_str_has_prefix(w->id, WINDOW_ID_PREFIX)) {
      xcb_window_t win = (xcb_window_t)strtoul(w->id + 4, NULL, 0);
      if (!g_hash_table_contains(seen, GUINT_TO_POINTER(win)))
        g_ptr_array_add(dead, (gpointer)w->id);
    }
  }
  g_list_free(all);
  for (guint i = 0; i < dead->len; i++)
    gcd_window_model_remove(b->model, g_ptr_array_index(dead, i));
  g_ptr_array_free(dead, TRUE);
  g_hash_table_unref(seen);
}

static void refresh_active(X11Backend *b)
{
  xcb_window_t active = (xcb_window_t)
      prop_cardinal(b, b->screen->root, A(_NET_ACTIVE_WINDOW), -1);
  if (active == b->active_win) return;
  b->active_win = active;
  /* re-flag: cheap enough to refetch every window's focus bit */
  GList *all = gcd_window_model_list(b->model);
  for (GList *l = all; l; l = l->next) {
    const GcdWindow *w = l->data;
    xcb_window_t win = (xcb_window_t)strtoul(w->id + 4, NULL, 0);
    gboolean focused = (win == active);
    gboolean has = w->flags & GCD_WINDOW_FOCUSED;
    if (focused != has) {
      GcdWindow upd = *w;
      upd.flags = focused ? (w->flags | GCD_WINDOW_FOCUSED)
                          : (w->flags & ~GCD_WINDOW_FOCUSED);
      /* upd aliases existing strings; upsert copies, no free needed */
      gcd_window_model_upsert(b->model, &upd);
    }
  }
  g_list_free(all);
}

/* ------------------------------------------------------------- events */

static void send_client_msg(X11Backend *b, xcb_window_t target_win,
                            xcb_atom_t type, uint32_t d0, uint32_t d1,
                            uint32_t d2)
{
  xcb_client_message_event_t ev = {0};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.window   = target_win;
  ev.type     = type;
  ev.format   = 32;
  ev.data.data32[0] = d0;
  ev.data.data32[1] = d1; /* timestamp / source */
  ev.data.data32[2] = d2;
  xcb_send_event(b->conn, 0, b->screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                 XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 (const char *)&ev);
  xcb_flush(b->conn);
}

static void handle_event(X11Backend *b, xcb_generic_event_t *ev)
{
  switch (ev->response_type & ~0x80) {
    case XCB_PROPERTY_NOTIFY: {
      xcb_property_notify_event_t *e = (xcb_property_notify_event_t *)ev;
      if (e->window == b->screen->root) {
        if (e->atom == A(_NET_CLIENT_LIST)) refresh_client_list(b);
        else if (e->atom == A(_NET_ACTIVE_WINDOW)) refresh_active(b);
      } else {
        fetch_window(b, e->window);
      }
      break;
    }
    case XCB_CONFIGURE_NOTIFY: {
      xcb_configure_notify_event_t *e =
          (xcb_configure_notify_event_t *)ev;
      if (e->window != b->screen->root && e->window != b->panel_xid)
        fetch_window(b, e->window);
      break;
    }
    case XCB_DESTROY_NOTIFY: {
      xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *)ev;
      g_autofree gchar *id =
          g_strdup_printf(WINDOW_ID_PREFIX "0x%x", e->window);
      gcd_window_model_remove(b->model, id);
      break;
    }
    case XCB_EXPOSE: {
      xcb_expose_event_t *e = (xcb_expose_event_t *)ev;
      if (b->panel_xid && e->window == b->panel_xid) {
        /* repaint requested: let the panel redraw on next present() —
         * notify via resize callback path */
        GList *all = gcd_window_model_list(b->model);
        g_list_free(all); /* model unchanged; expose handled below */
      }
      break;
    }
    default: break;
  }
}

static gboolean x11_io(GIOChannel *ch, GIOCondition cond, gpointer data)
{
  X11Backend *b = data;
  (void)ch;
  if (cond & (G_IO_ERR | G_IO_HUP)) {
    g_warning("x11 backend: connection lost");
    return G_SOURCE_REMOVE;
  }
  xcb_generic_event_t *ev;
  while ((ev = xcb_poll_for_event(b->conn))) {
    handle_event(b, ev);
    free(ev);
  }
  xcb_flush(b->conn);
  return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------- panel */

static xcb_visualtype_t *find_visual(X11Backend *b, xcb_visualid_t id)
{
  xcb_depth_iterator_t di =
      xcb_screen_allowed_depths_iterator(b->screen);
  for (; di.rem; xcb_depth_next(&di)) {
    xcb_visualtype_iterator_t vi =
        xcb_depth_visuals_iterator(di.data);
    for (; vi.rem; xcb_visualtype_next(&vi))
      if (vi.data->visual_id == id)
        return vi.data;
  }
  return NULL;
}

static cairo_surface_t *x11_get_cairo(GcdSurface *s)
{
  X11SurfacePriv *p = s->priv;
  return p->cs;
}

static void x11_present(GcdSurface *s)
{
  X11SurfacePriv *p = s->priv;
  X11Backend *b = (X11Backend *)s->backend;
  cairo_surface_flush(p->cs);
  xcb_flush(b->conn);
}

static void x11_surface_destroy(GcdSurface *s)
{
  X11SurfacePriv *p = s->priv;
  X11Backend *b = (X11Backend *)s->backend;
  if (p->cs) cairo_surface_destroy(p->cs);
  xcb_destroy_window(b->conn, p->xid);
  xcb_flush(b->conn);
  b->panel_xid = 0;
  g_free(p);
  g_free(s);
}

static GcdSurface *x11_panel_create(GcdBackend *base, guint height,
                                    GError **error)
{
  X11Backend *b = (X11Backend *)base;
  xcb_window_t win = xcb_generate_id(b->conn);
  guint width = b->screen->width_in_pixels;
  xcb_visualtype_t *visual =
      find_visual(b, b->screen->root_visual);

  if (!visual) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "x11: could not resolve root visual");
    return NULL;
  }

  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t values[] = {
    b->screen->black_pixel,
    XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
    XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
    XCB_EVENT_MASK_STRUCTURE_NOTIFY,
  };
  xcb_create_window(b->conn, XCB_COPY_FROM_PARENT, win,
                    b->screen->root, 0, 0, width, height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    b->screen->root_visual, mask, values);

  /* dock type + strut reservation */
  xcb_atom_t dock = A(_NET_WM_WINDOW_TYPE_DOCK);
  xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, win,
                      A(_NET_WM_WINDOW_TYPE), XCB_ATOM_ATOM, 32, 1, &dock);

  uint32_t strut[12] = {0};
  strut[2] = height;        /* top */
  strut[8] = 0;             /* top_start_x */
  strut[9] = width - 1;     /* top_end_x   */
  xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, win,
                      A(_NET_WM_STRUT_PARTIAL), XCB_ATOM_CARDINAL, 32,
                      12, strut);
  xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, win,
                      A(_NET_WM_STRUT), XCB_ATOM_CARDINAL, 32, 4, strut);

  const char *name = "gnome-c-desktop panel";
  xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, win,
                      A(_NET_WM_NAME), A(UTF8_STRING), 8,
                      strlen(name), name);
  xcb_atom_t st = A(_NET_WM_STATE_SKIP_TASKBAR);
  xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, win,
                      A(_NET_WM_STATE), XCB_ATOM_ATOM, 32, 1, &st);

  xcb_map_window(b->conn, win);
  xcb_flush(b->conn);
  b->panel_xid = win;

  GcdSurface *s = g_new0(GcdSurface, 1);
  X11SurfacePriv *p = g_new0(X11SurfacePriv, 1);
  p->xid = win;
  p->visual = visual;
  p->cs = cairo_xcb_surface_create(b->conn, win, visual, width, height);
  s->backend   = base;
  s->width     = width;
  s->height    = height;
  s->priv      = p;
  s->get_cairo = x11_get_cairo;
  s->present   = x11_present;
  s->destroy   = x11_surface_destroy;
  return s;
}

/* ------------------------------------------------------- backend iface */

static gboolean x11_window_op(GcdBackend *base, const GcdWindow *w,
                              GcdWindowOp op, gint arg, GError **error)
{
  X11Backend *b = (X11Backend *)base;
  if (!g_str_has_prefix(w->id, WINDOW_ID_PREFIX)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "x11 backend does not own window %s", w->id);
    return FALSE;
  }
  xcb_window_t win = (xcb_window_t)strtoul(w->id + 4, NULL, 0);
  switch (op) {
    case GCD_OP_ACTIVATE:
    case GCD_OP_UNMINIMIZE: /* activation also un-iconifies */
      send_client_msg(b, win, A(_NET_ACTIVE_WINDOW), 2 /* pager */, 0, 0);
      break;
    case GCD_OP_MINIMIZE:
      send_client_msg(b, win, A(WM_CHANGE_STATE), 3 /* IconicState */, 0, 0);
      break;
    case GCD_OP_CLOSE:
      send_client_msg(b, win, A(_NET_CLOSE_WINDOW), 0, 0, 0);
      break;
    case GCD_OP_MOVE_TO_WORKSPACE:
      send_client_msg(b, win, A(_NET_WM_DESKTOP), (uint32_t)arg, 2, 0);
      break;
  }
  return TRUE;
}

static gboolean x11_attach(GcdBackend *base, GMainContext *ctx,
                           GError **error)
{
  X11Backend *b = (X11Backend *)base;
  b->ctx = ctx;
  b->channel = g_io_channel_unix_new(xcb_get_file_descriptor(b->conn));
  b->watch_id = g_io_add_watch_full(b->channel, G_PRIORITY_DEFAULT,
                                    G_IO_IN | G_IO_ERR | G_IO_HUP,
                                    x11_io, b, NULL);
  g_io_channel_unref(b->channel);

  /* subscribe to root + do the initial population */
  uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                  XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
  xcb_change_window_attributes(b->conn, b->screen->root,
                               XCB_CW_EVENT_MASK, &mask);
  xcb_flush(b->conn);
  refresh_client_list(b);
  refresh_active(b);
  (void)error;
  return TRUE;
}

static GcdWindowModel *x11_model(GcdBackend *base)
{
  return ((X11Backend *)base)->model;
}

static void x11_destroy(GcdBackend *base)
{
  X11Backend *b = (X11Backend *)base;
  if (b->watch_id) g_source_remove(b->watch_id);
  if (b->model) gcd_window_model_free(b->model);
  if (b->conn) xcb_disconnect(b->conn);
  g_free(b);
}

static const GcdBackendIface x11_iface = {
  .name         = "x11",
  .attach       = x11_attach,
  .model        = x11_model,
  .window_op    = x11_window_op,
  .panel_create = x11_panel_create,
  .destroy      = x11_destroy,
};

GcdBackend *gcd_backend_x11_open(GError **error)
{
  X11Backend *b = g_new0(X11Backend, 1);
  b->conn = xcb_connect(NULL, &b->screen_no);
  if (xcb_connection_has_error(b->conn)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "cannot connect to X11 display %s",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
    xcb_disconnect(b->conn);
    g_free(b);
    return NULL;
  }
  const xcb_setup_t *setup = xcb_get_setup(b->conn);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < b->screen_no; i++) xcb_screen_next(&it);
  b->screen = it.data;
  intern_atoms(b);
  b->model = gcd_window_model_new();
  b->base.iface = &x11_iface;
  return (GcdBackend *)b;
}

gboolean gcd_backend_x11_probe(void)
{
  int err;
  xcb_connection_t *c = xcb_connect(NULL, NULL);
  err = xcb_connection_has_error(c);
  xcb_disconnect(c);
  return err == 0;
}
