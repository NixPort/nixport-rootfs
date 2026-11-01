#!/usr/bin/env bash
# PC test of the native-track gamepad shim (tools/fexpad/libfexpad.c): the shim
# gives the guest process a virtual gamepad, fed by the app's pad bridge.
# fakehost.py stands in for the app side (app/src/main/cpp/io/pad_bridge.c).
#
# Needs SDL3 (pkg-config sdl3). Without it the test is skipped (exit 77).
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT="${OUT:-build/fexpad-test}"
rm -rf "$OUT"
mkdir -p "$OUT"
ROOT="$OUT/root"
PADS="$ROOT/usr/share/fex-emu/fexdroid-input"
mkdir -p "$PADS"

CC=${CC:-cc}
CFLAGS="-Wall -Wextra -Werror -O1 -g"
LIBS="-ldl -lpthread"

if ! pkg-config --exists sdl3; then
  echo "skip: SDL3 not found (pkg-config sdl3)"
  exit 77
fi

$CC $CFLAGS -fPIC -shared -o "$OUT/libfexpad.so" tools/fexpad/libfexpad.c $LIBS
$CC $CFLAGS $(pkg-config --cflags sdl3) -o "$OUT/padtest" tests/fexpad/padtest.c $(pkg-config --libs sdl3)

python3 tests/fexpad/fakehost.py "$PADS" 200 tests/fexpad/events.txt 3 > "$OUT/host-a.log" 2>&1 &
HOST=$!
{ sleep 1; python3 tests/fexpad/fakehost.py "$PADS" 201 tests/fexpad/events-b.txt 1 > "$OUT/host-b.log" 2>&1; } &
HOST2=$!
NXP_HOST=""
cleanup() {
  if [ -n "$HOST" ] || [ -n "$HOST2" ]; then
    kill ${HOST:+"$HOST"} ${HOST2:+"$HOST2"} 2>/dev/null || true
  fi
  if [ -n "$NXP_HOST" ]; then
    kill "$NXP_HOST" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  [ -S "$PADS/event200" ] && break
  sleep 0.1
done
[ -S "$PADS/event200" ] || { echo "the fake host did not bind its socket"; cat "$OUT/host-a.log"; exit 1; }

# Linux takes LD_PRELOAD, macOS DYLD_INSERT_LIBRARIES.
case "$(uname -s)" in
  Darwin) INJECT="DYLD_INSERT_LIBRARIES=$OUT/libfexpad.so" ;;
  *)      INJECT="LD_PRELOAD=$OUT/libfexpad.so" ;;
esac

FXD_ROOT="$ROOT" FXD_PAD_SHIM_DEBUG=1 env $INJECT "$OUT/padtest" 6 > "$OUT/game.log" 2> "$OUT/shim.log" || true
wait $HOST 2>/dev/null || true
wait $HOST2 2>/dev/null || true
HOST=""
HOST2=""

cat "$OUT/game.log"
echo "--- shim log ---"
cat "$OUT/shim.log"

GAME="$OUT/game.log"
pass=0
fail=0
check() {
  local pattern="$1" what="$2"
  if grep -Eq "$pattern" "$GAME"; then
    echo "ok   $what"
    pass=$((pass + 1))
  else
    echo "FAIL $what (pattern: $pattern)"
    fail=$((fail + 1))
  fi
}

# An axis value with a tolerance: SDL normalizes the joystick axis with floats,
# so the game sees the trigger a LSB off the exact fraction.
check_axis() {
  local axis="$1" expected="$2" tolerance="$3" what="$4"
  local value
  # The peak value of that axis (the pad going away centres everything again).
  value=$(awk -v a="$axis" '$1 == "AXIS" && $3 == a { if ($4 > v) v = $4 } END { print v }' "$GAME")
  if [ -n "$value" ] && [ "$((value - expected))" -ge -"$tolerance" ] && [ "$((value - expected))" -le "$tolerance" ]; then
    echo "ok   $what ($value)"
    pass=$((pass + 1))
  else
    echo "FAIL $what (got '${value:-none}', wanted $expected +-$tolerance)"
    fail=$((fail + 1))
  fi
}

check '^GAMEPADS 1$' "the guest sees exactly one gamepad"
check '^GAMEPAD id=[0-9]+ name=Microsoft X-Box 360 pad vendor=045e product=028e axes=6 buttons=15 touchpads=0 fingers=0 hasTouchClick=0$' \
      "the device identity matches the x86 track (Xbox 360 pad)"
check '^BUTTON [0-9]+ 0 1$' "BTN_A became button 0 (SOUTH) pressed"
check '^BUTTON [0-9]+ 0 0$' "BTN_A release arrived"
check '^BUTTON [0-9]+ 11 1$' "the hat became the D-pad up button (11)"
check_axis 0 12345 16 "ABS_X became the left stick X axis"
check_axis 4 16448 16 "ABS_Z 128 became the left trigger axis (0..32767)"
check_axis 5 32767 16 "ABS_RZ 255 became the right trigger axis"
check '^BUTTON [0-9]+ 1 1$' "the second slot's BTN_B merged into the same gamepad (button 1)"
# Every input line carries the same instance id: two slots, one device.
ids=$(awk '$1 == "BUTTON" || $1 == "AXIS" { print $2 }' "$GAME" | sort -u | wc -l | tr -d ' ')
if [ "$ids" = "1" ]; then
  echo "ok   all input comes from one merged gamepad instance"
  pass=$((pass + 1))
else
  echo "FAIL expected input from a single gamepad instance, saw $ids"
  fail=$((fail + 1))
fi
removed=$(grep -c '^REMOVED' "$GAME" || true)
if [ "$removed" = "1" ]; then
  echo "ok   the device is withdrawn exactly once (when the last slot closes)"
  pass=$((pass + 1))
else
  echo "FAIL expected exactly one REMOVED, saw ${removed:-0}"
  fail=$((fail + 1))
fi

NXP_SOCKET="$OUT/nxp2.sock"
python3 tests/fexpad/fakehost.py --nxp2 "$NXP_SOCKET" > "$OUT/nxp2-host.log" 2>&1 &
NXP_HOST=$!
for _ in $(seq 1 50); do
  [ -S "$NXP_SOCKET" ] && break
  sleep 0.1
done
[ -S "$NXP_SOCKET" ] || { echo "the NXP2 fake host did not bind its socket"; cat "$OUT/nxp2-host.log"; exit 1; }

NXP_PAD_SOCKET="$NXP_SOCKET" FXD_PAD_SHIM_DEBUG=1 env $INJECT "$OUT/padtest" 5 \
    > "$OUT/nxp2-game.log" 2> "$OUT/nxp2-shim.log" || true
wait "$NXP_HOST"
NXP_HOST=""

echo
echo "--- NXP2 game ---"
cat "$OUT/nxp2-game.log"
echo "--- NXP2 host ---"
cat "$OUT/nxp2-host.log"
echo "--- NXP2 shim ---"
cat "$OUT/nxp2-shim.log"

GAME="$OUT/nxp2-game.log"
check '^GAMEPADS 2$' "NXP2 exposes two independent player devices"
touch_gamepads=$(grep -c 'name=Nixport Standard Gamepad .*touchpads=1 fingers=2 hasTouchClick=1$' "$GAME" || true)
if [ "$touch_gamepads" -ge 4 ]; then
  echo "ok   each player has two queried Touch fingers before and after reconnect"
  pass=$((pass + 1))
else
  echo "FAIL expected four Touch-capable device enumerations, saw ${touch_gamepads:-0}"
  fail=$((fail + 1))
fi
p1=$(awk '/^GAMEPAD id=/{sub("id=", "", $2); print $2; exit}' "$GAME")
p2=$(awk '/^GAMEPAD id=/{sub("id=", "", $2); if (++n == 2) { print $2; exit }}' "$GAME")
if [ -n "$p1" ] && [ -n "$p2" ] && [ "$p1" != "$p2" ] &&
   grep -Eq "^BUTTON $p1 4 1$" "$GAME" && grep -Eq "^BUTTON $p1 20 1$" "$GAME"; then
  echo "ok   Back and Touch click are independent on player 1"
  pass=$((pass + 1))
else
  echo "FAIL expected independent Back and Touch click events on player 1"
  fail=$((fail + 1))
fi
if [ -n "$p2" ] && grep -Eq "^BUTTON $p2 0 1$" "$GAME" &&
   ! grep -Eq "^BUTTON $p1 0 1$" "$GAME"; then
  echo "ok   player 2 input stays off player 1"
  pass=$((pass + 1))
else
  echo "FAIL expected SOUTH input only on player 2"
  fail=$((fail + 1))
fi
check "^TOUCH $p1 " "SDL emits touchpad contact events"
check "^TOUCH $p1 [0-9]+ 0 0\\.250 0\\.350 1\\.000$" \
      "finger identity remains stable after contact order changes"
check "^TOUCH $p1 [0-9]+ 1 0\\.650 0\\.750 1\\.000$" \
      "the second finger remains independently tracked"
check "^TOUCHSTATE $p1 [01] 1 " "SDL query reports an active touch with coordinates"
check "^TOUCHSTATE $p1 [01] 0 " "SDL query reports touch release"
check "^BUTTONSTATE $p1 20 1$" "SDL query reports standard Touch click down"
check "^BUTTONSTATE $p1 20 0$" "SDL query reports standard Touch click release"
if grep -q "NXP2 stale generation rejected and input cleared" "$OUT/nxp2-host.log" &&
   { grep -q "NXP2 transport disconnected" "$OUT/nxp2-host.log" ||
     grep -q "NXP2 abnormal message rejected and input cleared" "$OUT/nxp2-host.log"; } &&
   grep -q "NXP2 reconnect HELLO received" "$OUT/nxp2-host.log" &&
   grep -q "NXP2 READY generation=0x90a0b0c0" "$OUT/nxp2-host.log" &&
   [ "$(grep -c 'NXP2 ready:' "$OUT/nxp2-shim.log")" -ge 3 ]; then
  echo "ok   generation errors clear players and reconnect with a fresh session"
  pass=$((pass + 1))
else
  echo "FAIL expected generation rejection, abnormal disconnect, and reconnect"
  fail=$((fail + 1))
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
