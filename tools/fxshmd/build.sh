#!/usr/bin/env bash
# Cross-builds fxshmd (the SysV shm/semaphore registry daemon, see fxshmd.c) and the
# tests/sysvshm + tests/sysvsem test programs for the arm64 glibc rootfs with host clang + the
# Debian sysroot, like scripts/build-fex.sh does.  Output: build/fxshmd/.
# The ELF interpreter is left as Debian's; scripts/lib/fix-elf.py rewrites it
# to $FXD_LDSO when the payload is staged.
set -euo pipefail
cd "$(dirname "$0")/../.."
source scripts/config.sh
SYSROOT="$PWD/build/rootfs/sysroot-arm64"
[[ -d "$SYSROOT/usr/include" ]] || { echo "missing $SYSROOT (scripts/build-rootfs.sh)"; exit 1; }
OUT=build/fxshmd
mkdir -p "$OUT"
CC=(clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld -O2 -Wall -Wextra -fstack-protector-strong)
"${CC[@]}" -o "$OUT/fxshmd" tools/fxshmd/fxshmd.c
"${CC[@]}" -o "$OUT/shmtest" tests/sysvshm/shmtest.c
"${CC[@]}" -o "$OUT/semtest" tests/sysvsem/semtest.c
llvm-strip --strip-debug "$OUT/fxshmd" "$OUT/shmtest" "$OUT/semtest"
file "$OUT/fxshmd" "$OUT/shmtest" "$OUT/semtest" | sed 's/, BuildID.*//'
