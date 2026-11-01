#!/usr/bin/env bash
# The x86 multilib (amd64 + i386) guest rootfs for FEX, too big for the APK (~550 MB).
# Contains only Debian packages; programs are imported by the user. The app installs
# this tarball from a file (adb push / download), see session/X86Rootfs.kt.
#
# Output: build/rootfs/rootfs-x86-multilib.tar.zst (+ .packages.txt)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/rootfs
mkdir -p "$OUT"
APT_SNAPSHOT=""
[[ -n "${SNAPSHOT:-}" ]] && APT_SNAPSHOT="--snapshot $SNAPSHOT"

# steam-libs(-i386): Debian's dependency bundle for the Steam client (contrib/non-free);
# kept as the shortest way to pull the same GUI stack for both architectures. Plus the
# recommends that matter without a desktop: fonts, SDL, glib, xkbcommon, dbus.
PKGS=(
  libc6 libstdc++6 coreutils bash ca-certificates curl xz-utils file tar
  steam-libs steam-libs-i386
  libx11-6 libx11-6:i386 libxcb1 libxcb1:i386 libx11-xcb1 libx11-xcb1:i386
  libxkbcommon0 libxkbcommon-x11-0 libxss1 libxss1:i386 libxinerama1 libxinerama1:i386
  libxdamage1 libxfixes3 libxext6 libxext6:i386 libxrandr2 libxrender1 libxcomposite1 libxcursor1
  libsdl2-2.0-0 libsdl2-2.0-0:i386 libglib2.0-0t64 libglib2.0-0t64:i386 libfontconfig1 libfontconfig1:i386
  fontconfig fonts-liberation libdbus-1-3 libdbus-1-3:i386 dbus-daemon dbus-x11
  libpulse0 libpulse0:i386 pulseaudio-utils libnss3 libnspr4 libgtk-3-0t64 libgbm1 libegl1
  libvulkan1 libvulkan1:i386 vulkan-tools libusb-1.0-0 lsof procps
  libibus-1.0-5 libxtst6 libxtst6:i386 libvdpau1          # guest GUI/input stack (XTEST etc.)
)
cname="fexdroid-x86mlfs-$$"
docker rm -f "$cname" >/dev/null 2>&1 || true
docker run --platform linux/amd64 --name "$cname" debian:trixie-slim bash -euc "
  export DEBIAN_FRONTEND=noninteractive
  sed -i 's/^Components: main\$/Components: main contrib non-free non-free-firmware/' /etc/apt/sources.list.d/debian.sources
  dpkg --add-architecture i386
  apt-get update $APT_SNAPSHOT -qq
  apt-get install -y -qq --no-install-recommends ${PKGS[*]} >/dev/null
  apt-get clean
  rm -rf /var/lib/apt/lists/*
  dpkg-query -W -f='\${Package}:\${Architecture} \${Version}\n' > /packages.txt
"
rm -rf "$OUT/ml-stage" && mkdir -p "$OUT/ml-stage"
docker export "$cname" | tar -C "$OUT/ml-stage" -xf - 2>/dev/null || true
docker rm "$cname" >/dev/null
( cd "$OUT/ml-stage"
  rm -rf usr/share/doc/* usr/share/man/* usr/share/info/* usr/share/lintian var/log/* .dockerenv
  find usr/share/locale -mindepth 1 -maxdepth 1 ! -name 'locale.alias' -exec rm -rf {} + 2>/dev/null || true
)
# Guest programs (e.g. Chromium-based UIs) need a working /dev/shm; on the phone it is this directory.
mkdir -p "$OUT/ml-stage/dev/shm" "$OUT/ml-stage/run/pressure-vessel"
# No empty /proc or /sys: FEX would open those instead of the real ones (NOTES.md N-028).
rm -rf "$OUT/ml-stage/proc" "$OUT/ml-stage/sys"
chmod 1777 "$OUT/ml-stage/tmp" "$OUT/ml-stage/var/tmp" "$OUT/ml-stage/dev/shm"
mv "$OUT/ml-stage/packages.txt" "$OUT/rootfs-x86-multilib.packages.txt"
python3 scripts/lib/relativize-symlinks.py "$OUT/ml-stage"
mkdir -p "$OUT/ml-stage/opt/fexdroid-tests"
# --hard-dereference: Android apps cannot create hard links (SELinux), toybox tar stops at one.
tar -C "$OUT/ml-stage" --numeric-owner --owner=0 --group=0 --sort=name --hard-dereference -cf - . | zstd -T0 -19 -q -o "$OUT/rootfs-x86-multilib.tar.zst" -f
rm -rf "$OUT/ml-stage"
du -h "$OUT/rootfs-x86-multilib.tar.zst"
wc -l < "$OUT/rootfs-x86-multilib.packages.txt"
