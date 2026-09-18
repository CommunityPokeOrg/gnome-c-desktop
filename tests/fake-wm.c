/*
 * fake-wm.c — minimal EWMH "window manager" for the Xvfb smoke test.
 *
 * Creates two client windows, sets their EWMH properties, publishes
 * _NET_CLIENT_LIST on the root, then services _NET_ACTIVE_WINDOW /
 * _NET_CLOSE_WINDOW client messages, logging each op to stdout so the
 * test can assert the shell's window ops reach the WM.
 *
 * Usage: fake-wm <logfile>  (runs ~10s)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
  xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
      c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
  xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
  free(r);
  return a;
}

static void set_prop32(xcb_connection_t *c, xcb_window_t w,
                       xcb_atom_t p, xcb_atom_t t, uint32_t v)
{
  xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, p, t, 32, 1, &v);
}

static void set_text(xcb_connection_t *c, xcb_window_t w, xcb_atom_t p,
                     const char *v)
{
  xcb_atom_t utf8 = intern(c, "UTF8_STRING");
  xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, p, utf8, 8,
                      strlen(v), v);
}

int main(int argc, char **argv)
{
  xcb_connection_t *c = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(c)) { fprintf(stderr, "no X\n"); return 1; }
  xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  FILE *log = argc > 1 ? fopen(argv[1], "w") : stdout;

  xcb_atom_t A_CLIENT_LIST  = intern(c, "_NET_CLIENT_LIST");
  xcb_atom_t A_ACTIVE       = intern(c, "_NET_ACTIVE_WINDOW");
  xcb_atom_t A_WM_NAME      = intern(c, "_NET_WM_NAME");
  xcb_atom_t A_WM_DESKTOP   = intern(c, "_NET_WM_DESKTOP");
  xcb_atom_t A_WM_STATE     = intern(c, "_NET_WM_STATE");
  xcb_atom_t A_WM_TYPE      = intern(c, "_NET_WM_WINDOW_TYPE");
  xcb_atom_t A_TYPE_NORMAL  = intern(c, "_NET_WM_WINDOW_TYPE_NORMAL");
  xcb_atom_t A_CLOSE        = intern(c, "_NET_CLOSE_WINDOW");
  xcb_atom_t A_WM_CLASS     = intern(c, "WM_CLASS");
  xcb_atom_t A_WM_STATE_ICO = intern(c, "WM_CHANGE_STATE");

  xcb_window_t w1 = xcb_generate_id(c);
  xcb_window_t w2 = xcb_generate_id(c);
  uint32_t mv = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t vv[2] = { scr->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY };

  xcb_create_window(c, XCB_COPY_FROM_PARENT, w1, scr->root,
                    50, 50, 640, 480, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    scr->root_visual, mv, vv);
  xcb_create_window(c, XCB_COPY_FROM_PARENT, w2, scr->root,
                    200, 200, 400, 300, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    scr->root_visual, mv, vv);

  set_text(c, w1, A_WM_NAME, "Fake Terminal");
  set_text(c, w2, A_WM_NAME, "Fake Editor");
  set_text(c, w1, A_WM_CLASS, "term\0FakeTerm");
  set_text(c, w2, A_WM_CLASS, "edit\0FakeEdit");
  set_prop32(c, w1, A_WM_TYPE, XCB_ATOM_ATOM, A_TYPE_NORMAL);
  set_prop32(c, w2, A_WM_TYPE, XCB_ATOM_ATOM, A_TYPE_NORMAL);
  set_prop32(c, w1, A_WM_DESKTOP, XCB_ATOM_CARDINAL, 0);
  set_prop32(c, w2, A_WM_DESKTOP, XCB_ATOM_CARDINAL, 1);

  xcb_map_window(c, w1);
  xcb_map_window(c, w2);

  /* a WM gets client messages sent to root via Substructure masks */
  {
    uint32_t rootmask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(c, scr->root, XCB_CW_EVENT_MASK,
                                 &rootmask);
  }
  xcb_flush(c);

  /* publish the client list + a desktop count on root */
  xcb_window_t list[2] = { w1, w2 };
  xcb_change_property(c, XCB_PROP_MODE_REPLACE, scr->root,
                      A_CLIENT_LIST, XCB_ATOM_WINDOW, 32, 2, list);
  set_prop32(c, scr->root, intern(c, "_NET_NUMBER_OF_DESKTOPS"),
             XCB_ATOM_CARDINAL, 4);
  xcb_flush(c);
  fprintf(log, "fake-wm: ready w1=%x w2=%x\n", w1, w2);
  fflush(log);

  /* service WM duties for ~10s */
  for (int i = 0; i < 500; i++) {
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(c))) {
      if ((ev->response_type & ~0x80) == XCB_CLIENT_MESSAGE) {
        xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
        const char *what = "other";
        if (cm->type == A_ACTIVE)       what = "activate";
        else if (cm->type == A_CLOSE)   what = "close";
        else if (cm->type == A_WM_STATE_ICO) what = "minimize";
        else if (cm->type == A_WM_DESKTOP)   what = "move-ws";
        fprintf(log, "fake-wm: op=%s win=0x%x\n", what, cm->window);
        fflush(log);
        if (cm->type == A_ACTIVE)
          set_prop32(c, scr->root, A_ACTIVE, XCB_ATOM_WINDOW, cm->window);
      }
      free(ev);
    }
    usleep(20000);
  }
  xcb_disconnect(c);
  if (log != stdout) fclose(log);
  return 0;
}
