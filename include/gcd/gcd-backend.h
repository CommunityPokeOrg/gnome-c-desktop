/*
 * gcd-backend.h — display-server backend interface.
 *
 * Two implementations are built depending on platform libraries:
 *   - XCB + EWMH (any EWMH-compliant WM, incl. KWin on X11)
 *   - Wayland: wlr-layer-shell for the panel, wlr-foreign-toplevel for
 *     the window list; on compositors without those protocols (KWin
 *     Wayland) the KWin D-Bus bridge feeds the model instead.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>
#include <cairo.h>
#include "gcd-model.h"
#include "gcd-window.h"

G_BEGIN_DECLS

typedef struct GcdBackend GcdBackend;
typedef struct GcdSurface GcdSurface;

/* A drawable surface owned by a backend (panel dock window on X11,
 * layer-shell surface + shm buffer on Wayland). */
struct GcdSurface {
  GcdBackend *backend;
  guint       width;
  guint       height;

  /* A cairo surface valid until present(); drawing into it updates the
   * window content. For X11 this is a cairo-xcb surface (immediate);
   * for Wayland it is a cairo image surface over the shm buffer that
   * present() then attaches and commits. */
  cairo_surface_t *(*get_cairo) (GcdSurface *self);
  void             (*present)   (GcdSurface *self);
  void             (*destroy)   (GcdSurface *self);

  /* Wired up by the consumer (panel). May be NULL. */
  void (*on_pointer) (GcdSurface *self, gdouble x, gdouble y,
                      guint button, gboolean pressed, gpointer user_data);
  void (*on_resize)  (GcdSurface *self, guint width, guint height,
                      gpointer user_data);
  gpointer user_data;

  gpointer priv; /* backend-private */
};

typedef struct {
  const gchar *name; /* "x11" | "wayland" */

  /* Hook the backend's fds into `ctx` (GLib main context). */
  gboolean (*attach)     (GcdBackend *self, GMainContext *ctx, GError **error);

  /* The model the backend maintains. Owned by the backend. */
  GcdWindowModel *(*model) (GcdBackend *self);

  /* Apply `op` to a window the backend reported (id has this backend's
   * prefix). Returns FALSE with GError if the window is not managed by
   * this backend or the operation failed. */
  gboolean (*window_op)  (GcdBackend *self, const GcdWindow *window,
                          GcdWindowOp op, gint arg, GError **error);

  /* Create a top-anchored panel surface (dock window / layer surface). */
  GcdSurface *(*panel_create) (GcdBackend *self, guint height,
                               GError **error);

  void (*destroy) (GcdBackend *self);
} GcdBackendIface;

struct GcdBackend {
  const GcdBackendIface *iface;
};

/* Open the named backend ("x11" or "wayland"), or the best available one
 * when `name` is "auto" or NULL. */
GcdBackend *gcd_backend_open  (const gchar *name, GError **error);
gboolean    gcd_backend_probe (const gchar *name); /* usable without opening */

/* TRUE when the backend itself can enumerate toplevel windows. FALSE
 * for a Wayland compositor without the foreign-toplevel protocol — the
 * caller should then feed the model from the KWin bridge. */
gboolean    gcd_backend_has_toplevel_source (GcdBackend *backend);

G_END_DECLS
