/*
 * gcd-backend-wayland.c — Wayland backend.
 *
 *   Panel       : zwlr_layer_shell_v1, top layer, exclusive zone,
 *                 drawn into a wl_shm buffer via cairo (image surface).
 *   Window list : zwlr_foreign_toplevel_manager_v1 (wlroots protocol).
 *                 Compositors without it — notably KWin on Wayland —
 *                 report no toplevel source; gcd-shell then feeds the
 *                 model from the KWin D-Bus bridge instead (see
 *                 src/kwin/gcd-kwin.c), which is what enables the
 *                 cross-KWin sync scenario.
 *   Input       : wl_seat pointer events forwarded to the surface that
 *                 the pointer entered.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cairo.h>
#include <gio/gio.h>
#include <wayland-client.h>

#include "gcd/gcd-backend.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define WINDOW_ID_PREFIX "ftl:"

typedef struct WlBackend   WlBackend;
typedef struct WlPanel     WlPanel;
typedef struct WlToplevel  WlToplevel;

struct WlToplevel {
  WlBackend *backend;
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  gchar  *id;
  gchar  *title;
  gchar  *app_id;
  guint   flags;
  gboolean closed;
};

struct WlPanel {
  GcdSurface     base;
  WlBackend     *backend;
  struct wl_surface            *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct wl_buffer             *buffer;
  cairo_surface_t              *cs;
  void                         *pixels;
  guint                         width, height;
  gboolean                      configured;
  gboolean                      dirty;
};

struct WlBackend {
  GcdBackend  base;
  struct wl_display  *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm        *shm;
  struct wl_seat       *seat;
  struct wl_pointer    *pointer;
  struct zwlr_layer_shell_v1               *layer_shell;
  struct zwlr_foreign_toplevel_manager_v1  *ftl_mgr;
  GcdWindowModel *model;
  GHashTable *toplevels; /* wl handle ptr -> WlToplevel* */
  GHashTable *surfaces;  /* wl_surface*  -> WlPanel* */
  GIOChannel *channel;
  guint watch_id;
  guint ftl_seq;
  WlPanel *pointer_panel;
};

/* ------------------------------------------------------------ shm buffer */

static int create_shm_fd(off_t size)
{
  int fd = memfd_create("gcd-shm", MFD_CLOEXEC);
  if (fd < 0) {
    char tmpl[] = "/dev/shm/gcd-XXXXXX";
    fd = mkstemp(tmpl);
    if (fd >= 0) unlink(tmpl);
  }
  if (fd < 0) return -1;
  if (ftruncate(fd, size) < 0) { close(fd); return -1; }
  return fd;
}

static gboolean panel_alloc_buffer(WlPanel *p)
{
  guint stride = p->width * 4;
  off_t size = stride * p->height;
  int fd;
  struct wl_shm_pool *pool;

  if (p->buffer)  wl_buffer_destroy(p->buffer);
  if (p->cs)      cairo_surface_destroy(p->cs);
  if (p->pixels)  munmap(p->pixels, size);
  p->buffer = NULL; p->cs = NULL; p->pixels = NULL;

  fd = create_shm_fd(size);
  if (fd < 0) return FALSE;
  p->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p->pixels == MAP_FAILED) { close(fd); p->pixels = NULL; return FALSE; }
  pool = wl_shm_create_pool(p->backend->shm, fd, size);
  p->buffer = wl_shm_pool_create_buffer(pool, 0, p->width, p->height,
                                        stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  p->cs = cairo_image_surface_create_for_data(
      p->pixels, CAIRO_FORMAT_ARGB32, p->width, p->height, stride);
  memset(p->pixels, 0, size);
  return TRUE;
}

/* ------------------------------------------------------ layer surface */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h)
{
  WlPanel *p = data;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  if (w > 0) p->width = w;
  if (h > 0) p->height = h;
  if (!p->buffer || !p->configured)
    panel_alloc_buffer(p);
  p->configured = TRUE;
  if (p->base.on_resize)
    p->base.on_resize(&p->base, p->width, p->height, p->base.user_data);
  p->dirty = TRUE;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
  (void)data; (void)ls;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
  .configure = layer_configure,
  .closed    = layer_closed,
};

/* ------------------------------------------------- foreign toplevel */

static void tl_title(void *data,
                     struct zwlr_foreign_toplevel_handle_v1 *h,
                     const char *title)
{
  WlToplevel *t = data;
  g_free(t->title);
  t->title = g_strdup(title);
  (void)h;
}

static void tl_app_id(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *h,
                      const char *app_id)
{
  WlToplevel *t = data;
  g_free(t->app_id);
  t->app_id = g_strdup(app_id);
  (void)h;
}

static void tl_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *o) { (void)d; (void)h; (void)o; }
static void tl_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *o) { (void)d; (void)h; (void)o; }
static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                      struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void)d; (void)h; (void)p; }

static void tl_state(void *data,
                     struct zwlr_foreign_toplevel_handle_v1 *h,
                     struct wl_array *states)
{
  WlToplevel *t = data;
  guint flags = 0;
  uint32_t *s;
  wl_array_for_each(s, states) {
    switch (*s) {
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
        flags |= GCD_WINDOW_FOCUSED; break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
        flags |= GCD_WINDOW_MINIMIZED; break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
        flags |= GCD_WINDOW_MAXIMIZED; break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
        flags |= GCD_WINDOW_FULLSCREEN; break;
    }
  }
  t->flags = flags;
  (void)h;
}

static void tl_done(void *data,
                    struct zwlr_foreign_toplevel_handle_v1 *h)
{
  WlToplevel *t = data;
  GcdWindow rec = {0};
  if (t->closed) return;
  rec.id        = t->id;
  rec.title     = t->title;
  rec.app_id    = t->app_id;
  rec.flags     = t->flags;
  rec.workspace = -1;
  rec.origin    = "local";
  gcd_window_model_upsert(t->backend->model, &rec);
  (void)h;
}

static void tl_closed(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *h)
{
  WlToplevel *t = data;
  t->closed = TRUE;
  gcd_window_model_remove(t->backend->model, t->id);
  zwlr_foreign_toplevel_handle_v1_destroy(h);
  g_hash_table_remove(t->backend->toplevels, h);
}

static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
  .title        = tl_title,
  .app_id       = tl_app_id,
  .output_enter = tl_output_enter,
  .output_leave = tl_output_leave,
  .state        = tl_state,
  .done         = tl_done,
  .closed       = tl_closed,
  .parent       = tl_parent,
};

static void ftl_toplevel(void *data,
                         struct zwlr_foreign_toplevel_manager_v1 *mgr,
                         struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  WlBackend *b = data;
  WlToplevel *t = g_new0(WlToplevel, 1);
  t->backend = b;
  t->handle  = handle;
  t->id      = g_strdup_printf(WINDOW_ID_PREFIX "%u", ++b->ftl_seq);
  g_hash_table_insert(b->toplevels, handle, t);
  zwlr_foreign_toplevel_handle_v1_add_listener(handle, &tl_listener, t);
  (void)mgr;
}

static void ftl_finished(void *data,
                         struct zwlr_foreign_toplevel_manager_v1 *mgr)
{
  (void)data;
  zwlr_foreign_toplevel_manager_v1_destroy(mgr);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener ftl_listener = {
  .toplevel = ftl_toplevel,
  .finished = ftl_finished,
};

/* ------------------------------------------------------------- pointer */

static void ptr_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
  WlBackend *b = data;
  b->pointer_panel = surface ? g_hash_table_lookup(b->surfaces, surface) : NULL;
  (void)ptr; (void)serial; (void)x; (void)y;
}

static void ptr_leave(void *data, struct wl_pointer *ptr, uint32_t serial,
                      struct wl_surface *surface)
{
  WlBackend *b = data;
  b->pointer_panel = NULL;
  (void)ptr; (void)serial; (void)surface;
}

static void ptr_motion(void *data, struct wl_pointer *ptr, uint32_t t,
                       wl_fixed_t x, wl_fixed_t y)
{
  WlBackend *b = data;
  WlPanel *p = b->pointer_panel;
  if (p && p->base.on_pointer)
    p->base.on_pointer(&p->base, wl_fixed_to_double(x),
                       wl_fixed_to_double(y), 0, FALSE, p->base.user_data);
  (void)ptr; (void)t;
}

static void ptr_button(void *data, struct wl_pointer *ptr, uint32_t serial,
                       uint32_t t, uint32_t button, uint32_t state)
{
  WlBackend *b = data;
  WlPanel *p = b->pointer_panel;
  /* wl_pointer has no x/y in button events; motion always precedes. */
  if (p && p->base.on_pointer)
    p->base.on_pointer(&p->base, -1, -1, button,
                       state == WL_POINTER_BUTTON_STATE_PRESSED,
                       p->base.user_data);
  (void)ptr; (void)serial; (void)t;
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a,
                     wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
  .enter        = ptr_enter,
  .leave        = ptr_leave,
  .motion       = ptr_motion,
  .button       = ptr_button,
  .axis         = ptr_axis,
  .frame        = ptr_frame,
  .axis_source  = ptr_axis_src,
  .axis_stop    = ptr_axis_stop,
  .axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
  WlBackend *b = data;
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !b->pointer) {
    b->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(b->pointer, &pointer_listener, b);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && b->pointer) {
    wl_pointer_destroy(b->pointer);
    b->pointer = NULL;
  }
}

static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = {
  .capabilities = seat_capabilities,
  .name         = seat_name,
};

/* ----------------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface,
                            uint32_t version)
{
  WlBackend *b = data;
  if (strcmp(iface, wl_compositor_interface.name) == 0)
    b->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                   MIN(version, 4));
  else if (strcmp(iface, wl_shm_interface.name) == 0)
    b->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  else if (strcmp(iface, wl_seat_interface.name) == 0) {
    b->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                               MIN(version, 5));
    wl_seat_add_listener(b->seat, &seat_listener, b);
  } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
    b->layer_shell = wl_registry_bind(reg, name,
                                      &zwlr_layer_shell_v1_interface, 1);
  else if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
    b->ftl_mgr = wl_registry_bind(
        reg, name, &zwlr_foreign_toplevel_manager_v1_interface,
        MIN(version, 3));
    zwlr_foreign_toplevel_manager_v1_add_listener(b->ftl_mgr,
                                                  &ftl_listener, b);
  }
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }

static const struct wl_registry_listener registry_listener = {
  .global        = registry_global,
  .global_remove = registry_remove,
};

/* ------------------------------------------------------------ dispatch */

static gboolean wl_io(GIOChannel *ch, GIOCondition cond, gpointer data)
{
  WlBackend *b = data;
  (void)ch;
  if (cond & (G_IO_ERR | G_IO_HUP)) {
    g_warning("wayland backend: display connection lost");
    return G_SOURCE_REMOVE;
  }
  if (wl_display_dispatch(b->display) < 0) {
    g_warning("wayland backend: dispatch failed: %s", g_strerror(errno));
    return G_SOURCE_REMOVE;
  }
  while (wl_display_flush(b->display) < 0) {
    if (errno != EAGAIN) break;
    wl_display_dispatch_pending(b->display);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean wl_attach(GcdBackend *base, GMainContext *ctx, GError **err)
{
  WlBackend *b = (WlBackend *)base;
  (void)ctx;
  b->channel = g_io_channel_unix_new(wl_display_get_fd(b->display));
  b->watch_id = g_io_add_watch(b->channel, G_IO_IN | G_IO_ERR | G_IO_HUP,
                               wl_io, b);
  g_io_channel_unref(b->channel);
  /* a second roundtrip picks up globals + initial toplevels */
  wl_display_roundtrip(b->display);
  if (!b->ftl_mgr)
    g_message("wayland: no foreign-toplevel protocol — window list must "
              "come from another source (e.g. the KWin bridge)");
  if (!b->layer_shell)
    g_message("wayland: no layer-shell protocol — panel unavailable");
  (void)err;
  return TRUE;
}

static GcdWindowModel *wl_model(GcdBackend *base)
{ return ((WlBackend *)base)->model; }

static WlToplevel *find_toplevel(WlBackend *b, const gchar *id)
{
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, b->toplevels);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    WlToplevel *t = v;
    if (g_strcmp0(t->id, id) == 0)
      return t;
  }
  return NULL;
}

static gboolean wl_window_op(GcdBackend *base, const GcdWindow *w,
                             GcdWindowOp op, gint arg, GError **error)
{
  WlBackend *b = (WlBackend *)base;
  WlToplevel *t;
  if (!g_str_has_prefix(w->id, WINDOW_ID_PREFIX)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "wayland backend does not own window %s", w->id);
    return FALSE;
  }
  t = find_toplevel(b, w->id);
  if (!t) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "window %s no longer exists", w->id);
    return FALSE;
  }
  switch (op) {
    case GCD_OP_ACTIVATE:
      if (!b->seat) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "no seat available for activation");
        return FALSE;
      }
      zwlr_foreign_toplevel_handle_v1_activate(t->handle, b->seat);
      break;
    case GCD_OP_MINIMIZE:
      zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
      break;
    case GCD_OP_UNMINIMIZE:
      zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
      break;
    case GCD_OP_CLOSE:
      zwlr_foreign_toplevel_handle_v1_close(t->handle);
      break;
    case GCD_OP_MOVE_TO_WORKSPACE:
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "workspace moves are not part of foreign-toplevel v1");
      return FALSE;
  }
  wl_display_flush(b->display);
  (void)arg;
  return TRUE;
}

/* ------------------------------------------------------------ surfaces */

static cairo_surface_t *wl_get_cairo(GcdSurface *s)
{
  return ((WlPanel *)s)->cs;
}

static void wl_present(GcdSurface *s)
{
  WlPanel *p = (WlPanel *)s;
  if (!p->configured || !p->buffer) return;
  cairo_surface_flush(p->cs);
  wl_surface_attach(p->surface, p->buffer, 0, 0);
  wl_surface_damage_buffer(p->surface, 0, 0, p->width, p->height);
  wl_surface_commit(p->surface);
  wl_display_flush(p->backend->display);
  p->dirty = FALSE;
}

static void wl_panel_surface_destroy(GcdSurface *s)
{
  WlPanel *p = (WlPanel *)s;
  WlBackend *b = p->backend;
  g_hash_table_remove(b->surfaces, p->surface);
  if (b->pointer_panel == p) b->pointer_panel = NULL;
  if (p->layer_surface) zwlr_layer_surface_v1_destroy(p->layer_surface);
  if (p->surface)       wl_surface_destroy(p->surface);
  if (p->buffer)        wl_buffer_destroy(p->buffer);
  if (p->cs)            cairo_surface_destroy(p->cs);
  if (p->pixels)        munmap(p->pixels, (size_t)p->width * 4 * p->height);
  wl_display_flush(b->display);
  g_free(p);
}

static GcdSurface *wl_panel_create(GcdBackend *base, guint height,
                                   GError **error)
{
  WlBackend *b = (WlBackend *)base;
  WlPanel *p;
  if (!b->layer_shell || !b->compositor || !b->shm) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "compositor lacks wlr-layer-shell; cannot dock a panel");
    return NULL;
  }
  p = g_new0(WlPanel, 1);
  p->backend = b;
  p->width   = 1; /* stretched by anchors; configure tells real width */
  p->height  = height;
  p->surface = wl_compositor_create_surface(b->compositor);
  p->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      b->layer_shell, p->surface, NULL,
      ZWLR_LAYER_SHELL_V1_LAYER_TOP, "gnome-c-desktop-panel");
  zwlr_layer_surface_v1_add_listener(p->layer_surface,
                                     &layer_listener, p);
  zwlr_layer_surface_v1_set_anchor(
      p->layer_surface,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(p->layer_surface, 0, height);
  zwlr_layer_surface_v1_set_exclusive_zone(p->layer_surface, height);
  wl_surface_commit(p->surface);
  wl_display_roundtrip(b->display);

  p->base.backend    = base;
  p->base.width      = p->width;
  p->base.height     = height;
  p->base.get_cairo  = wl_get_cairo;
  p->base.present    = wl_present;
  p->base.destroy    = wl_panel_surface_destroy;
  p->base.priv       = p;
  g_hash_table_insert(b->surfaces, p->surface, p);
  return &p->base;
}

static void wl_destroy(GcdBackend *base)
{
  WlBackend *b = (WlBackend *)base;
  if (b->watch_id) g_source_remove(b->watch_id);
  if (b->ftl_mgr) zwlr_foreign_toplevel_manager_v1_destroy(b->ftl_mgr);
  if (b->layer_shell) zwlr_layer_shell_v1_destroy(b->layer_shell);
  if (b->pointer) wl_pointer_destroy(b->pointer);
  if (b->seat) wl_seat_destroy(b->seat);
  if (b->shm) wl_shm_destroy(b->shm);
  if (b->compositor) wl_compositor_destroy(b->compositor);
  if (b->registry) wl_registry_destroy(b->registry);
  if (b->toplevels) g_hash_table_unref(b->toplevels);
  if (b->surfaces) g_hash_table_unref(b->surfaces);
  if (b->model) gcd_window_model_free(b->model);
  if (b->display) wl_display_disconnect(b->display);
  g_free(b);
}

static const GcdBackendIface wl_iface = {
  .name         = "wayland",
  .attach       = wl_attach,
  .model        = wl_model,
  .window_op    = wl_window_op,
  .panel_create = wl_panel_create,
  .destroy      = wl_destroy,
};

static void toplevel_free(WlToplevel *t)
{
  if (!t) return;
  g_free(t->id);
  g_free(t->title);
  g_free(t->app_id);
  g_free(t);
}

GcdBackend *gcd_backend_wayland_open(GError **error)
{
  WlBackend *b = g_new0(WlBackend, 1);
  b->display = wl_display_connect(NULL);
  if (!b->display) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "cannot connect to Wayland display %s",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY")
                                        : "(unset)");
    g_free(b);
    return NULL;
  }
  b->model     = gcd_window_model_new();
  b->toplevels = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, (GDestroyNotify)toplevel_free);
  b->surfaces  = g_hash_table_new(g_direct_hash, g_direct_equal);
  b->registry  = wl_display_get_registry(b->display);
  wl_registry_add_listener(b->registry, &registry_listener, b);
  wl_display_roundtrip(b->display); /* bind globals */
  b->base.iface = &wl_iface;
  return (GcdBackend *)b;
}

gboolean gcd_backend_wayland_probe(void)
{
  struct wl_display *d = wl_display_connect(NULL);
  if (!d) return FALSE;
  wl_display_disconnect(d);
  return TRUE;
}

gboolean gcd_backend_wayland_has_toplevel_source(GcdBackend *base)
{
  return ((WlBackend *)base)->ftl_mgr != NULL;
}
