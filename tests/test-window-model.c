/* test-window-model.c — model semantics + listener ordering. MIT. */
#include "gcd/gcd-window.h"
#include "gcd/gcd-model.h"

static gint adds, changes, removes;
static gchar *last_id;

static void cb_added(GcdWindowModel *m, const GcdWindow *w, gpointer ud)
{ (void)m; (void)ud; adds++; g_free(last_id); last_id = g_strdup(w->id); }
static void cb_changed(GcdWindowModel *m, const GcdWindow *w, gpointer ud)
{ (void)m; (void)ud; changes++; }
static void cb_removed(GcdWindowModel *m, const gchar *id, gpointer ud)
{ (void)m; (void)ud; removes++; g_free(last_id); last_id = g_strdup(id); }

static void test_upsert_remove(void)
{
  GcdWindowModel *m = gcd_window_model_new();
  GcdWindowModelListener l = { cb_added, cb_changed, cb_removed, NULL };
  gcd_window_model_add_listener(m, &l);

  GcdWindow *w = gcd_window_new("x11:0x1");
  w->title = g_strdup("term");
  gcd_window_model_upsert(m, w);
  g_assert_cmpint(adds, ==, 1);
  g_assert_cmpint(changes, ==, 0);
  g_assert_cmpstr(last_id, ==, "x11:0x1");

  /* identical upsert → no event */
  gcd_window_model_upsert(m, w);
  g_assert_cmpint(changes, ==, 0);

  /* changed title → changed event */
  g_free(w->title);
  w->title = g_strdup("term2");
  gcd_window_model_upsert(m, w);
  g_assert_cmpint(changes, ==, 1);
  g_assert_cmpstr(gcd_window_model_find(m, "x11:0x1")->title, ==, "term2");

  g_assert_true(gcd_window_model_remove(m, "x11:0x1"));
  g_assert_cmpint(removes, ==, 1);
  g_assert_false(gcd_window_model_remove(m, "x11:0x1"));
  g_assert_cmpint(removes, ==, 1);

  gcd_window_free(w);
  gcd_window_model_free(m);
}

static void test_purge_origin(void)
{
  GcdWindowModel *m = gcd_window_model_new();
  gcd_window_model_add_listener(m, &(GcdWindowModelListener){
      cb_added, cb_changed, cb_removed, NULL});

  for (int i = 0; i < 3; i++) {
    gchar idbuf[16];
    g_snprintf(idbuf, sizeof(idbuf), "w%d", i);
    GcdWindow *w = gcd_window_new(idbuf);
    g_free(w->origin);
    w->origin = g_strdup(i < 2 ? "peerA" : "local");
    gcd_window_model_upsert(m, w);
    gcd_window_free(w);
  }
  g_assert_cmpint(gcd_window_model_size(m), ==, 3);
  guint n = gcd_window_model_purge_origin(m, "peerA");
  g_assert_cmpint(n, ==, 2);
  g_assert_cmpint(gcd_window_model_size(m), ==, 1);
  gcd_window_model_free(m);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/model/upsert-remove", test_upsert_remove);
  g_test_add_func("/model/purge-origin", test_purge_origin);
  return g_test_run();
}
