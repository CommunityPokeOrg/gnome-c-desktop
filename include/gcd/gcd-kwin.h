/*
 * gcd-kwin.h — KWin D-Bus bridge.
 *
 * On KWin (either session type, but especially Wayland where there is
 * no generic foreign-toplevel protocol) this module loads a small KWin
 * script — src/kwin/gcd-kwin-bridge.js, installed to
 * $prefix/share/gnome-c-desktop — which:
 *
 *   1. serializes workspace.windowList() and pushes full snapshots +
 *      deltas to this process over D-Bus (method Sink/SnapshotDone on
 *      org.gcd.KWinBridge<pid>/org/gcd/KWinBridge), and
 *   2. polls us for queued window commands (activate, minimize, close,
 *      move-to-desktop) and applies them to the real KWin windows.
 *
 * The result feeds the local GcdWindowModel with ids "kwin:{uuid}", and
 * window operations route back through the same bridge — so nothing is
 * ever reopened or rerun.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <gio/gio.h>
#include "gcd-model.h"
#include "gcd-window.h"

G_BEGIN_DECLS

typedef struct GcdKwinBridge GcdKwinBridge;

/* TRUE when org.kde.KWin is owned on the session bus. */
gboolean       gcd_kwin_available    (GError **error);

/* Create a bridge: claims org.gcd.KWinBridge<pid>, exports the D-Bus
 * object, loads + starts the KWin script, and begins filling `model`
 * (origin = "local", ids "kwin:<uuid>"). */
GcdKwinBridge *gcd_kwin_bridge_new   (GcdWindowModel *model,
                                      const gchar    *script_path,
                                      GError        **error);
void           gcd_kwin_bridge_free  (GcdKwinBridge *bridge);

/* Queue a window operation for the KWin script to apply on next poll. */
gboolean       gcd_kwin_bridge_window_op (GcdKwinBridge *bridge,
                                          const gchar   *window_id,
                                          GcdWindowOp    op,
                                          gint           arg,
                                          GError       **error);

/* Locate the installed bridge script (checks GCD_KWIN_BRIDGE_SCRIPT env,
 * ./src/kwin for dev builds, then $prefix/share/gnome-c-desktop). */
gchar         *gcd_kwin_bridge_script_path (void);

G_END_DECLS
