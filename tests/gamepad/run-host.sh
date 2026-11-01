#!/usr/bin/env bash
# PC test of the gamepad path (NOTES N-054): app bridge + FEX side, natively.
# Needs the FEX source with patch 0006 applied (scripts/build-fex.sh leaves it in build/fex/src).
set -euo pipefail
cd "$(dirname "$0")/../.."
FEXLS=build/fex/src/Source/Tools/LinuxEmulation
[[ -f $FEXLS/LinuxSyscalls/AndroidInputPad.h ]] || { echo "run scripts/build-fex.sh first (patch 0006)"; exit 1; }
JNI_INC=$(dirname "$(ls "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h | tail -1)")
OUT=build/gamepad-test
mkdir -p "$OUT/jni"
cp "$JNI_INC/jni.h" "$OUT/jni/"   # only jni.h: the rest of the NDK sysroot is bionic
W="-Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined"
clang $W -Itests/gamepad/stubs -I"$OUT/jni" -c app/src/main/cpp/io/pad_bridge.c -o "$OUT/pad_bridge.o"
clang++ $W -std=c++20 -Itests/gamepad/stubs -I"$FEXLS" -c tests/gamepad/padclient.cpp -o "$OUT/padclient.o"
clang $W -I"$OUT/jni" -c tests/gamepad/hosttest.c -o "$OUT/hosttest.o"
clang++ $W "$OUT"/pad_bridge.o "$OUT"/padclient.o "$OUT"/hosttest.o -lpthread -o "$OUT/hosttest"
"$OUT/hosttest"
