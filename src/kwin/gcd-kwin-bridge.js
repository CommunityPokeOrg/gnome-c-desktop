/*
 * gcd-kwin-bridge.js — KWin script loaded by gcd-kwin.c.
 *
 * Pushes window snapshots to the gcd sync node over D-Bus and polls it
 * for queued commands to apply to the live windows (activate, minimize,
 * close, move-to-desktop). No application is ever (re)started here.
 *
 * @GCD_SERVICE@ and @GCD_OBJPATH@ are substituted by gcd-kwin.c before
 * the script is handed to org.kde.kwin.Scripting.loadScript.
 *
 * Compatible with the KWin 5.27+/6 scripting API (workspace.windowList,
 * window.internalId, callDBus). SPDX-License-Identifier: MIT
 */

var GCD_SERVICE = "@GCD_SERVICE@";
var GCD_OBJPATH = "@GCD_OBJPATH@";
var GCD_IFACE   = "org.gcd.KWinBridge";

function gcdCall(method, arg, replyCb) {
  try {
    if (replyCb) {
      callDBus(GCD_SERVICE, GCD_OBJPATH, GCD_IFACE, method, arg, replyCb);
    } else {
      callDBus(GCD_SERVICE, GCD_OBJPATH, GCD_IFACE, method, arg);
    }
  } catch (e) {
    print("gcd-kwin-bridge: callDBus " + method + " failed: " + e);
  }
}

function serializeWindow(w) {
  var ws = -1;
  try {
    if (w.desktops && w.desktops.length > 0)
      ws = 1; /* KWin6: desktops are objects; report first index-agnostic */
    if (typeof w.desktop === "number")
      ws = w.desktop; /* KWin5 */
  } catch (e) {}
  var flags = 0;
  try {
    if (w === workspace.activeWindow) flags |= 1;   /* FOCUSED */
    if (w.minimized)   flags |= 2;                   /* MINIMIZED */
    if (w.fullScreen)  flags |= 8;                   /* FULLSCREEN */
    if (w.keepAbove)   flags |= 64;                  /* ON_TOP */
    if (w.skipTaskbar) flags |= 32;                  /* SKIP_TASKBAR */
  } catch (e) {}
  var g = w.frameGeometry;
  return {
    id:        "kwin:" + String(w.internalId),
    app_id:    String(w.resourceClass || w.desktopFileName || ""),
    title:     String(w.caption || ""),
    x:         g ? g.x : 0,
    y:         g ? g.y : 0,
    width:     g ? g.width : 0,
    height:    g ? g.height : 0,
    workspace: ws,
    flags:     flags,
    origin:    "local"
  };
}

function pushSnapshot() {
  try {
    var list = workspace.windowList();
    for (var i = 0; i < list.length; i++) {
      var w = list[i];
      if (!w.normalWindow && !w.dialog) continue; /* panels, docks, etc. */
      gcdCall("Sink", JSON.stringify(serializeWindow(w)));
    }
    gcdCall("SnapshotDone", "");
  } catch (e) {
    print("gcd-kwin-bridge: pushSnapshot failed: " + e);
  }
}

function applyCommand(cmd) {
  try {
    var list = workspace.windowList();
    for (var i = 0; i < list.length; i++) {
      var w = list[i];
      if (String(w.internalId) !== String(cmd.id)) continue;
      switch (cmd.op) {
        case "activate":
          workspace.activeWindow = w;
          break;
        case "unminimize":
          w.minimized = false;
          workspace.activeWindow = w;
          break;
        case "minimize":
          w.minimized = true;
          break;
        case "close":
          w.closeWindow();
          break;
        case "move_to_workspace":
          if (w.desktops && workspace.desktops &&
              cmd.arg >= 0 && cmd.arg < workspace.desktops.length)
            w.desktops = [workspace.desktops[cmd.arg]];
          else if (typeof w.desktop === "number")
            w.desktop = cmd.arg;
          break;
      }
      return;
    }
  } catch (e) {
    print("gcd-kwin-bridge: applyCommand failed: " + e);
  }
}

function pollCommands() {
  gcdCall("Poll", "", function (reply) {
    try {
      if (!reply) return;
      var cmds = JSON.parse(reply);
      for (var i = 0; i < cmds.length; i++)
        applyCommand(cmds[i]);
    } catch (e) {
      print("gcd-kwin-bridge: bad Poll reply: " + e);
    }
  });
}

/* ----------------------------------------------------- wiring */

var pushPending = false;
function schedulePush() {
  if (pushPending) return;
  pushPending = true;
  /* debounce bursts (title spam, mass changes) */
  delayTimer.restart();
}

var delayTimer = new QTimer();
delayTimer.singleShot = true;
delayTimer.interval = 120;
delayTimer.timeout.connect(function () {
  pushPending = false;
  pushSnapshot();
});

var pollTimer = new QTimer();
pollTimer.interval = 250;
pollTimer.timeout.connect(pollCommands);
pollTimer.start();

try {
  workspace.windowAdded.connect(schedulePush);
  workspace.windowRemoved.connect(schedulePush);
  if (workspace.windowActivated)
    workspace.windowActivated.connect(schedulePush);
} catch (e) {
  print("gcd-kwin-bridge: signal wiring failed: " + e);
}

pushSnapshot();
print("gcd-kwin-bridge: loaded, service=" + GCD_SERVICE);
