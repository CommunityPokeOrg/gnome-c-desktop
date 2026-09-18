/*
 * gcd-model.h — GcdWindowModel: the observable window registry.
 *
 * One model is fed by the local backend ("local model"). A second model
 * ("merged model") is maintained by the sync layer and additionally
 * contains windows replicated from remote peers, distinguished by
 * GcdWindow.origin. The panel renders the merged model.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>
#include "gcd-window.h"

G_BEGIN_DECLS

typedef struct GcdWindowModel GcdWindowModel;

typedef struct {
  /* `window` is owned by the model and valid only for the duration of
   * the call. `id` is passed on removal because the record is already
   * gone by then. */
  void (*added)   (GcdWindowModel *model, const GcdWindow *window,
                   gpointer user_data);
  void (*changed) (GcdWindowModel *model, const GcdWindow *window,
                   gpointer user_data);
  void (*removed) (GcdWindowModel *model, const gchar *id,
                   gpointer user_data);
  gpointer user_data;
} GcdWindowModelListener;

GcdWindowModel *gcd_window_model_new  (void);
void            gcd_window_model_free (GcdWindowModel *model);

void  gcd_window_model_add_listener    (GcdWindowModel *model,
                                        const GcdWindowModelListener *listener);

/* Insert or update (the model copies). Fires `added` for a new id,
 * `changed` when an existing record actually differs. */
void  gcd_window_model_upsert          (GcdWindowModel *model,
                                        const GcdWindow *window);

gboolean            gcd_window_model_remove  (GcdWindowModel *model,
                                              const gchar    *id);
const GcdWindow    *gcd_window_model_find    (GcdWindowModel *model,
                                              const gchar    *id);
guint               gcd_window_model_size    (GcdWindowModel *model);

/* Newly-allocated GList of const GcdWindow* (owned by the model), in a
 * stable order (by id). Free the list only. */
GList              *gcd_window_model_list    (GcdWindowModel *model);

/* Remove every window whose origin matches `origin` (each removal fires
 * `removed`). Used when a sync peer drops. */
guint               gcd_window_model_purge_origin (GcdWindowModel *model,
                                                   const gchar    *origin);

G_END_DECLS
