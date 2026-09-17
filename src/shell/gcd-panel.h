/*
 * gcd-panel.h — internal: the GNOME-style top bar widget.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>
#include "gcd/gcd-backend.h"
#include "gcd/gcd-sync.h"
#include "gcd/gcd-model.h"

typedef struct GcdPanel GcdPanel;

/* Requested op on a taskbar entry (may be a remote window — the sync
 * node routes it). */
typedef void (*GcdPanelOpCb)(const gchar *window_id, GcdWindowOp op,
                             gint arg, gpointer user_data);

GcdPanel *gcd_panel_new     (GcdSurface     *surface,
                             GcdWindowModel *merged_model,
                             GcdPanelOpCb    op_cb,
                             gpointer        op_user_data);
void      gcd_panel_free    (GcdPanel *panel);
void      gcd_panel_repaint (GcdPanel *panel);
/* optional status text rendered at the right end ("peers: 2") */
void      gcd_panel_set_status (GcdPanel *panel, const gchar *status);
