#!/bin/sh
# x11-smoke.sh — end-to-end sync test across TWO separate Xvfb displays:
#
#   :X1  fake-wm (2 EWMH windows) + gcd-shell dispA  (owns the windows)
#   :X2                            gcd-shell dispB  (peer of dispA)
#
# Assertions:
#   1. dispB's merged model shows dispA's windows tagged [dispA]
#   2. `gcd-ctl activate` on dispB reaches the WM on dispA (op=activate)
#      — i.e. the window was driven remotely without relaunching it.
#
# args: $1 = meson build dir (contains gcd-shell, gcd-ctl, fake-wm)
set -u
B="$1"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}/gcd-test-$$"
mkdir -p "$RUNTIME"
SOCK_A="$RUNTIME/a.sock"
SOCK_B="$RUNTIME/b.sock"
WM_LOG="$RUNTIME/wm.log"
OUT_B="$RUNTIME/out-b.txt"
PIDS=""

cleanup() {
  for p in $PIDS; do kill "$p" 2>/dev/null; done
  # every spawned process has $RUNTIME in its argv (sock/log paths)
  pkill -f "$RUNTIME" 2>/dev/null
  rm -rf "$RUNTIME"
}
trap cleanup EXIT

# --- display A: fake WM + shell A -------------------------------------
xvfb-run -a sh -c "
  '$B/fake-wm' '$WM_LOG' &
  sleep 0.5
  exec '$B/gcd-shell' --backend x11 --no-panel --instance dispA \
      --sync-sock '$SOCK_A'
" & PIDS="$PIDS $!"
sleep 2

# --- display B: shell B peering to A ----------------------------------
xvfb-run -a "$B/gcd-shell" --backend x11 --no-panel --instance dispB \
    --sync-sock "$SOCK_B" --peer "unix:$SOCK_A" & PIDS="$PIDS $!"
sleep 2

# --- verify ------------------------------------------------------------
"$B/gcd-ctl" --sock "$SOCK_B" list > "$OUT_B" || {
  echo "FAIL: gcd-ctl list failed"; cat "$OUT_B"; exit 1; }

ID=$(grep 'Fake Editor' "$OUT_B" | head -1 | cut -d' ' -f1)
echo "activating $ID on dispB (owned by dispA)"
[ -n "$ID" ] && "$B/gcd-ctl" --sock "$SOCK_B" activate "$ID"
sleep 0.8

grep -q "Fake Terminal" "$OUT_B" || { echo "FAIL: B did not see Fake Terminal"; cat "$OUT_B"; exit 1; }
grep -q "Fake Editor"   "$OUT_B" || { echo "FAIL: B did not see Fake Editor";   cat "$OUT_B"; exit 1; }
grep -q "\[dispA\]"     "$OUT_B" || { echo "FAIL: remote origin not tagged";    cat "$OUT_B"; exit 1; }
grep -q "op=activate"   "$WM_LOG" || { echo "FAIL: WM never got activate";      cat "$WM_LOG"; exit 1; }

echo "x11-smoke: PASS"
exit 0
