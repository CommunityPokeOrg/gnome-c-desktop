/*
 * gcd-model.c — GcdWindowModel implementation.
 * SPDX-License-Identifier: MIT
 */

#include "gcd/gcd-model.h"

struct GcdWindowModel {
  GHashTable *windows; /* id (borrowed from GcdWindow) -> GcdWindow* */
  GPtrArray  *listeners;
};

GcdWindowModel *gcd_window_model_new(void)
{
  GcdWindowModel *m = g_new0(GcdWindowModel, 1);
  m->windows = g_hash_table_new_full(g_str_hash, g_str_equal,
                                     NULL, (GDestroyNotify)gcd_window_free);
  m->listeners = g_ptr_array_new_with_free_func(g_free);
  return m;
}

void gcd_window_model_free(GcdWindowModel *m)
{
  if (!m) return;
  g_hash_table_unref(m->windows);
  g_ptr_array_unref(m->listeners);
  g_free(m);
}

void gcd_window_model_add_listener(GcdWindowModel *m,
                                   const GcdWindowModelListener *l)
{
  g_return_if_fail(m && l);
  g_ptr_array_add(m->listeners,
                  g_memdup2(l, sizeof(GcdWindowModelListener)));
}

static void emit_added(GcdWindowModel *m, const GcdWindow *w)
{
  for (guint i = 0; i < m->listeners->len; i++) {
    GcdWindowModelListener *l = g_ptr_array_index(m->listeners, i);
    if (l->added) l->added(m, w, l->user_data);
  }
}

static void emit_changed(GcdWindowModel *m, const GcdWindow *w)
{
  for (guint i = 0; i < m->listeners->len; i++) {
    GcdWindowModelListener *l = g_ptr_array_index(m->listeners, i);
    if (l->changed) l->changed(m, w, l->user_data);
  }
}

static void emit_removed(GcdWindowModel *m, const gchar *id)
{
  for (guint i = 0; i < m->listeners->len; i++) {
    GcdWindowModelListener *l = g_ptr_array_index(m->listeners, i);
    if (l->removed) l->removed(m, id, l->user_data);
  }
}

void gcd_window_model_upsert(GcdWindowModel *m, const GcdWindow *w)
{
  GcdWindow *existing;
  g_return_if_fail(m && w && w->id);

  existing = g_hash_table_lookup(m->windows, w->id);
  if (!existing) {
    GcdWindow *copy = gcd_window_copy(w);
    g_hash_table_insert(m->windows, copy->id, copy);
    emit_added(m, copy);
    return;
  }
  if (!gcd_window_same_state(existing, w)) {
    g_free(existing->app_id); existing->app_id = g_strdup(w->app_id);
    g_free(existing->title);  existing->title  = g_strdup(w->title);
    g_free(existing->origin); existing->origin = g_strdup(w->origin);
    existing->x = w->x; existing->y = w->y;
    existing->width = w->width; existing->height = w->height;
    existing->workspace = w->workspace;
    existing->flags = w->flags;
    emit_changed(m, existing);
  }
}

gboolean gcd_window_model_remove(GcdWindowModel *m, const gchar *id)
{
  gchar *dup;
  g_return_val_if_fail(m && id, FALSE);
  if (!g_hash_table_contains(m->windows, id))
    return FALSE;
  /* the stored key is freed by remove() — report a copy */
  dup = g_strdup(id);
  g_hash_table_remove(m->windows, id);
  emit_removed(m, dup);
  g_free(dup);
  return TRUE;
}

const GcdWindow *gcd_window_model_find(GcdWindowModel *m, const gchar *id)
{
  g_return_val_if_fail(m && id, NULL);
  return g_hash_table_lookup(m->windows, id);
}

guint gcd_window_model_size(GcdWindowModel *m)
{
  g_return_val_if_fail(m, 0);
  return g_hash_table_size(m->windows);
}

static gint by_id(gconstpointer a, gconstpointer b)
{
  const GcdWindow *wa = a, *wb = b;
  return g_strcmp0(wa->id, wb->id);
}

GList *gcd_window_model_list(GcdWindowModel *m)
{
  GList *list = NULL;
  GHashTableIter it;
  gpointer v;
  g_return_val_if_fail(m, NULL);
  g_hash_table_iter_init(&it, m->windows);
  while (g_hash_table_iter_next(&it, NULL, &v))
    list = g_list_prepend(list, v);
  return g_list_sort(list, by_id);
}

guint gcd_window_model_purge_origin(GcdWindowModel *m, const gchar *origin)
{
  GPtrArray *dead;
  GHashTableIter it;
  gpointer k, v;
  guint n = 0;

  g_return_val_if_fail(m && origin, 0);
  dead = g_ptr_array_new();
  g_hash_table_iter_init(&it, m->windows);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    const GcdWindow *w = v;
    if (g_strcmp0(w->origin, origin) == 0)
      g_ptr_array_add(dead, k);
  }
  for (guint i = 0; i < dead->len; i++) {
    gchar *id = g_ptr_array_index(dead, i);
    gchar *dup = g_strdup(id);
    g_hash_table_remove(m->windows, id);
    emit_removed(m, dup);
    g_free(dup);
    n++;
  }
  g_ptr_array_free(dead, TRUE);
  return n;
}
