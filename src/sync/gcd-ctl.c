/*
 * gcd-ctl.c — command-line client for a running shell/sync node.
 *
 *   gcd-ctl [--sock PATH] list
 *   gcd-ctl [--sock PATH] <op> <window-id> [arg]
 *   gcd-ctl [--sock PATH] ping
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <gio/gunixsocketaddress.h>
#include "gcd/gcd-sync.h"
#include "gcd/gcd-json.h"

static gchar *opt_sock = NULL;

static const GOptionEntry opts[] = {
  { "sock", 's', 0, G_OPTION_ARG_STRING, &opt_sock,
    "Sync unix socket path", "PATH" },
  { NULL }
};

static GSocketConnection *connect_sync(GError **error)
{
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_autoptr(GSocketAddress) addr =
      g_unix_socket_address_new(opt_sock ? opt_sock
                                         : gcd_sync_default_socket_path());
  return g_socket_client_connect(client, addr, NULL, error);
}

static gboolean send_msg(GSocketConnection *conn, GcdSyncMsg *m,
                         GError **error)
{
  gchar *line = gcd_sync_msg_encode(m);
  gboolean ok = g_output_stream_write_all(
      g_io_stream_get_output_stream(G_IO_STREAM(conn)),
      line, strlen(line), NULL, NULL, error);
  g_free(line);
  return ok;
}

static gchar *read_line(GSocketConnection *conn, GError **error)
{
  /* read until LF, bounded */
  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
  GString *buf = g_string_new(NULL);
  char c;
  for (;;) {
    gssize n = g_input_stream_read(in, &c, 1, NULL, error);
    if (n <= 0) { g_string_free(buf, TRUE); return NULL; }
    if (c == '\n') break;
    g_string_append_c(buf, c);
    if (buf->len > 1024 * 1024) {
      g_string_free(buf, TRUE);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "line too long");
      return NULL;
    }
  }
  return g_string_free(buf, FALSE);
}

static int cmd_list(void)
{
  g_autoptr(GError) err = NULL;
  GSocketConnection *conn = connect_sync(&err);
  GcdSyncMsg m;
  gchar *line;

  if (!conn) { g_printerr("connect: %s\n", err->message); return 1; }

  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_SUBSCRIBE;
  if (!send_msg(conn, &m, &err)) {
    g_printerr("send: %s\n", err->message);
    return 1;
  }

  /* skip HELLO (and whatever else) until a SNAPSHOT arrives */
  for (;;) {
    GcdSyncMsg in;
    line = read_line(conn, &err);
    if (!line) { g_printerr("read: %s\n", err->message); return 1; }
    if (!gcd_sync_msg_decode(line, &in, &err)) {
      g_printerr("decode: %s\n", err->message);
      g_free(line);
      return 1;
    }
    g_free(line);
    if (in.type == GCD_SYNC_MSG_SNAPSHOT) {
      for (guint i = 0; i < (in.windows ? in.windows->len : 0); i++) {
        GcdWindow *w = g_ptr_array_index(in.windows, i);
        g_autofree gchar *s = gcd_window_summary(w);
        g_print("%s\n", s);
      }
      gcd_sync_msg_clear(&in);
      break;
    }
    gcd_sync_msg_clear(&in);
  }
  g_object_unref(conn);
  return 0;
}

static int cmd_op(const gchar *opname, const gchar *id, gint arg)
{
  GcdWindowOp op;
  g_autoptr(GError) err = NULL;
  GSocketConnection *conn;
  GcdSyncMsg m;

  if (!gcd_window_op_from_name(opname, &op)) {
    g_printerr("unknown op '%s' (try activate|minimize|unminimize|close|"
               "move_to_workspace)\n", opname);
    return 2;
  }
  conn = connect_sync(&err);
  if (!conn) { g_printerr("connect: %s\n", err->message); return 1; }
  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_COMMAND;
  m.id = (gchar *)id;
  m.op = op;
  m.arg = arg;
  if (!send_msg(conn, &m, &err)) {
    g_printerr("send: %s\n", err->message);
    return 1;
  }
  g_print("sent %s %s\n", opname, id);
  g_object_unref(conn);
  return 0;
}

static int cmd_ping(void)
{
  g_autoptr(GError) err = NULL;
  GSocketConnection *conn = connect_sync(&err);
  GcdSyncMsg m;
  gchar *line;
  int rc = 0;

  if (!conn) { g_printerr("connect: %s\n", err->message); return 1; }
  gcd_sync_msg_init(&m);
  m.type = GCD_SYNC_MSG_PING;
  if (!send_msg(conn, &m, &err)) {
    g_printerr("send: %s\n", err->message);
    return 1;
  }
  /* first inbound line may be HELLO — read until PONG */
  for (int i = 0; i < 8; i++) {
    GcdSyncMsg in;
    line = read_line(conn, &err);
    if (!line) { g_printerr("read: %s\n", err->message); return 1; }
    gcd_sync_msg_decode(line, &in, NULL);
    g_free(line);
    if (in.type == GCD_SYNC_MSG_PONG) {
      g_print("pong\n");
      gcd_sync_msg_clear(&in);
      goto done;
    }
    gcd_sync_msg_clear(&in);
  }
  g_printerr("no pong\n");
  rc = 1;
done:
  g_object_unref(conn);
  return rc;
}

int main(int argc, char **argv)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GOptionContext) oc;

  oc = g_option_context_new("<list|ping|OP WINDOW-ID [ARG]>");
  g_option_context_add_main_entries(oc, opts, NULL);
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("%s\n", err->message);
    return 2;
  }
  if (argc < 2) {
    g_printerr("usage: %s\n", g_option_context_get_help(oc, TRUE, NULL));
    return 2;
  }
  if (g_strcmp0(argv[1], "list") == 0) return cmd_list();
  if (g_strcmp0(argv[1], "ping") == 0) return cmd_ping();
  if (argc >= 3) return cmd_op(argv[1], argv[2],
                               argc >= 4 ? atoi(argv[3]) : 0);
  g_printerr("usage: gcd-ctl <list|ping|OP WINDOW-ID [ARG]>\n");
  return 2;
}
