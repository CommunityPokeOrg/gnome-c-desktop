/* test-sync-proto.c — message encode/decode round-trips. MIT. */
#include <string.h>
#include "gcd/gcd-sync.h"

static void test_hello(void)
{
  GcdSyncMsg m, out;
  gchar *line;
  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_HELLO;
  m.version = GCD_SYNC_PROTOCOL_VERSION;
  m.instance = (gchar *)"host-a:wayland-0";
  line = gcd_sync_msg_encode(&m);
  g_assert_nonnull(strchr(line, '\n'));
  g_assert_true(gcd_sync_msg_decode(line, &out, NULL));
  g_assert_cmpint(out.type, ==, GCD_SYNC_MSG_HELLO);
  g_assert_cmpstr(out.instance, ==, "host-a:wayland-0");
  gcd_sync_msg_clear(&out);
  g_free(line);
}

static void test_snapshot_roundtrip(void)
{
  GcdSyncMsg m, out;
  gchar *line;
  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_SNAPSHOT;
  m.windows = g_ptr_array_new_with_free_func(
      (GDestroyNotify)gcd_window_free);
  GcdWindow *w = gcd_window_new("x11:0x42");
  w->title = g_strdup("Terminal — editor");
  w->app_id = g_strdup("gnome-terminal");
  w->x = 10; w->y = 20; w->width = 800; w->height = 600;
  w->workspace = 2;
  w->flags = GCD_WINDOW_FOCUSED | GCD_WINDOW_MAXIMIZED;
  g_ptr_array_add(m.windows, w);

  line = gcd_sync_msg_encode(&m);
  g_assert_true(gcd_sync_msg_decode(line, &out, NULL));
  g_assert_cmpint(out.type, ==, GCD_SYNC_MSG_SNAPSHOT);
  g_assert_cmpint(out.windows->len, ==, 1);
  const GcdWindow *rw = g_ptr_array_index(out.windows, 0);
  g_assert_cmpstr(rw->id, ==, "x11:0x42");
  g_assert_cmpstr(rw->title, ==, "Terminal — editor");
  g_assert_cmpint(rw->workspace, ==, 2);
  g_assert_cmpuint(rw->flags, ==,
                   GCD_WINDOW_FOCUSED | GCD_WINDOW_MAXIMIZED);

  gcd_sync_msg_clear(&out);
  g_ptr_array_unref(m.windows);
  g_free(line);
}

static void test_command(void)
{
  GcdSyncMsg m, out;
  gchar *line;
  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_COMMAND;
  m.id = (gchar *)"kwin:{abc}";
  m.op = GCD_OP_MOVE_TO_WORKSPACE;
  m.arg = 3;
  line = gcd_sync_msg_encode(&m);
  g_assert_true(gcd_sync_msg_decode(line, &out, NULL));
  g_assert_cmpint(out.type, ==, GCD_SYNC_MSG_COMMAND);
  g_assert_cmpint(out.op, ==, GCD_OP_MOVE_TO_WORKSPACE);
  g_assert_cmpint(out.arg, ==, 3);
  g_assert_cmpstr(out.id, ==, "kwin:{abc}");
  gcd_sync_msg_clear(&out);
  g_free(line);
}

static void test_garbage(void)
{
  GcdSyncMsg m;
  GError *e = NULL;
  g_assert_false(gcd_sync_msg_decode("not json", &m, &e));
  g_clear_error(&e);
  g_assert_false(gcd_sync_msg_decode("{\"type\":\"bogus\"}", &m, &e));
  g_clear_error(&e);
  g_assert_false(
      gcd_sync_msg_decode("{\"type\":\"upsert\"}", &m, &e));
  g_clear_error(&e);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/sync/hello", test_hello);
  g_test_add_func("/sync/snapshot", test_snapshot_roundtrip);
  g_test_add_func("/sync/command", test_command);
  g_test_add_func("/sync/garbage", test_garbage);
  return g_test_run();
}
