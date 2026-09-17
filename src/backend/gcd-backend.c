/*
 * gcd-backend.c — backend selection.
 * SPDX-License-Identifier: MIT
 */

#include <gio/gio.h>
#include "gcd/gcd-backend.h"
#include "gcd-config.h"

GcdBackend *gcd_backend_x11_open(GError **error);
GcdBackend *gcd_backend_wayland_open(GError **error);
gboolean    gcd_backend_x11_probe(void);
gboolean    gcd_backend_wayland_probe(void);
gboolean    gcd_backend_wayland_has_toplevel_source(GcdBackend *base);

gboolean gcd_backend_probe(const gchar *name)
{
#if GCD_HAVE_X11
  if (g_strcmp0(name, "x11") == 0) return gcd_backend_x11_probe();
#endif
#if GCD_HAVE_WAYLAND
  if (g_strcmp0(name, "wayland") == 0) return gcd_backend_wayland_probe();
#endif
  return FALSE;
}

GcdBackend *gcd_backend_open(const gchar *name, GError **error)
{
  if (!name || g_strcmp0(name, "auto") == 0) {
#if GCD_HAVE_WAYLAND
    if (getenv("WAYLAND_DISPLAY"))
      return gcd_backend_wayland_open(error);
#endif
#if GCD_HAVE_X11
    if (getenv("DISPLAY"))
      return gcd_backend_x11_open(error);
#endif
#if GCD_HAVE_WAYLAND
    return gcd_backend_wayland_open(error);
#elif GCD_HAVE_X11
    return gcd_backend_x11_open(error);
#else
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "no display backend was built");
    return NULL;
#endif
  }
#if GCD_HAVE_X11
  if (g_strcmp0(name, "x11") == 0)
    return gcd_backend_x11_open(error);
#endif
#if GCD_HAVE_WAYLAND
  if (g_strcmp0(name, "wayland") == 0)
    return gcd_backend_wayland_open(error);
#endif
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
              "unknown or unavailable backend '%s'", name);
  return NULL;
}

gboolean gcd_backend_has_toplevel_source(GcdBackend *backend)
{
#if GCD_HAVE_WAYLAND
  if (g_strcmp0(backend->iface->name, "wayland") == 0)
    return gcd_backend_wayland_has_toplevel_source(backend);
#endif
  return TRUE; /* EWMH always lists clients */
}
