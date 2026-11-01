#!/usr/bin/env bash
# Builds a tarball with gdb (+ the Debian packages it pulls in beyond the runtime rootfs)
# for on-device debugging. Not shipped in the APK; push it with:
#   scripts/adb-push-rootfs.sh build/debugtools.tar tmp/debugtools.tar
#   scripts/adb-run.sh 'tar -C $FXD_ROOT -xf $FXD_ROOT/tmp/debugtools.tar'
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh
OUT=build/debugtools
rm -rf "$OUT" && mkdir -p "$OUT"
docker run --rm --platform linux/arm64 -v "$PWD/$OUT:/out" debian:trixie-slim bash -euc '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  dpkg-query -W -f="\${Package}\n" | sort > /before.txt
  apt-get install -y -qq --no-install-recommends gdb >/dev/null
  dpkg-query -W -f="\${Package}\n" | sort > /after.txt
  comm -13 /before.txt /after.txt > /out/packages.txt
  for p in $(cat /out/packages.txt); do dpkg -L "$p"; done | sort -u | while read -r f; do
    [ -f "$f" ] || [ -L "$f" ] || continue
    case "$f" in /usr/share/doc/*|/usr/share/man/*|/usr/share/locale/*|/usr/share/info/*) continue;; esac
    echo "${f#/}"
  done > /files.txt
  tar -C / --numeric-owner -cf /out/raw.tar -T /files.txt
  chown -R '"$(id -u):$(id -g)"' /out
'
mkdir -p "$OUT/stage" && tar -C "$OUT/stage" -xf "$OUT/raw.tar"
python3 scripts/lib/fix-elf.py "$OUT/stage" "$FXD_ROOT" "$FXD_LDSO"
tar -C "$OUT/stage" --numeric-owner --owner=0 --group=0 -cf build/debugtools.tar .
echo "packages: $(tr '\n' ' ' < "$OUT/packages.txt")"
du -h build/debugtools.tar
