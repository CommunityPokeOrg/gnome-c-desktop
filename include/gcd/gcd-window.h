/*
 * gcd-window.h — window object shared by every backend and the sync layer.
 *
 * A GcdWindow is a plain data record. Backends create them from native
 * window state (EWMH properties, foreign-toplevel events, KWin D-Bus
 * snapshots); the sync layer serializes them so a peer shell can show —
 * and act on — windows that live on another display without reopening
 * the underlying application.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  GCD_WINDOW_FOCUSED      = 1 << 0,
  GCD_WINDOW_MINIMIZED    = 1 << 1,
  GCD_WINDOW_MAXIMIZED    = 1 << 2,
  GCD_WINDOW_FULLSCREEN   = 1 << 3,
  GCD_WINDOW_URGENT       = 1 << 4,
  GCD_WINDOW_SKIP_TASKBAR = 1 << 5,
  GCD_WINDOW_ON_TOP       = 1 << 6,
} GcdWindowFlags;

/* Operations a shell may request on a window. These never launch or
 * re-launch anything — they are applied to the live window on the
 * display that owns it. */
typedef enum {
  GCD_OP_ACTIVATE = 0,          /* raise + focus on its display */
  GCD_OP_MINIMIZE,              /* iconify */
  GCD_OP_UNMINIMIZE,            /* restore */
  GCD_OP_CLOSE,                 /* graceful close request */
  GCD_OP_MOVE_TO_WORKSPACE,     /* arg = workspace index */
} GcdWindowOp;

typedef struct {
  gchar *id;        /* stable opaque id, prefixed by source:
                     *   "x11:0x3a00007"   — X11 window id
                     *   "ftl:12"          — foreign-toplevel seq (wayland)
                     *   "kwin:{uuid}"     — KWin internal id */
  gchar *app_id;    /* WM_CLASS res_class / app_id / resourceClass */
  gchar *title;     /* _NET_WM_NAME / toplevel title / caption */
  gint   x, y;
  gint   width, height;
  gint   workspace; /* -1 = on all workspaces */
  guint  flags;     /* GcdWindowFlags bitmask */
  gchar *origin;    /* "local" or the remote instance name it synced from */
} GcdWindow;

GcdWindow *gcd_window_new          (const gchar     *id);
GcdWindow *gcd_window_copy         (const GcdWindow *window);
void       gcd_window_free         (GcdWindow       *window);

/* TRUE when everything except the id/origin is identical — used by the
 * model to suppress redundant "changed" notifications. */
gboolean   gcd_window_same_state   (const GcdWindow *a,
                                    const GcdWindow *b);

/* Compact one-line description, for `--dump-model` and logs. */
gchar     *gcd_window_summary      (const GcdWindow *window);

const gchar *gcd_window_op_name    (GcdWindowOp      op);
gboolean     gcd_window_op_from_name (const gchar *name, GcdWindowOp *op);

G_END_DECLS
