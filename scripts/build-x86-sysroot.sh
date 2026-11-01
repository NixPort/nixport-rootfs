#!/usr/bin/env bash
# Build the Debian 13 (trixie) amd64 *development* sysroot used to cross-compile the
# x86_64 guest side of the FEX thunks (scripts/build-fex.sh, BUILD_THUNKS=ON).
# It is a build-time input only; nothing from it ships to the phone (the guest
# runtime rootfs is build/rootfs/rootfs-x86_64.tar from scripts/build-rootfs.sh).
#
# Outputs (in build/rootfs/):
#   sysroot-x86_64/               headers, .so/.a, .pc files, GCC runtime (crt*, libgcc)
#   sysroot-x86_64.packages.txt   exact package versions (reproducibility record)
#
# Set SNAPSHOT=YYYYMMDDTHHMMSSZ to pin the Debian archive via snapshot.debian.org.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/rootfs
IMAGE="debian:trixie-slim"
mkdir -p "$OUT"

APT_SNAPSHOT=""
[[ -n "${SNAPSHOT:-}" ]] && APT_SNAPSHOT="--snapshot $SNAPSHOT"

# Everything ThunkLibs/GuestLibs/CMakeLists.txt asks pkg-config / the compiler for.
# clang --target=x86_64-linux-gnu --sysroot=<this> needs the GCC runtime pieces
# (libgcc-14-dev: crtbegin.o, libgcc) and libstdc++ headers from the sysroot.
X86_DEV_PKGS=(
  libc6-dev libstdc++-14-dev libgcc-14-dev
  libasound2-dev                                  # asound thunk
  libx11-dev libx11-xcb-dev libxcb1-dev libxrandr-dev libxrender-dev   # vulkan/GL thunks (xcb;x11;xrandr;xrender)
  libdrm-dev                                      # drm thunk
  libwayland-dev                                  # wayland-client thunk
  libgl-dev libegl-dev                            # GL/EGL thunks (built, not enabled on device)
)

cname="fexdroid-sysroot-x86_64-$$"
docker rm -f "$cname" >/dev/null 2>&1 || true
docker run --platform linux/amd64 --name "$cname" "$IMAGE" bash -euc "
  export DEBIAN_FRONTEND=noninteractive
  apt-get update $APT_SNAPSHOT -qq
  apt-get install -y -qq --no-install-recommends ${X86_DEV_PKGS[*]} >/dev/null
  apt-get clean
  rm -rf /var/lib/apt/lists/*
  dpkg-query -W -f='\${Package} \${Version}\n' > /packages.txt
"
docker export "$cname" > "$OUT/sysroot-x86_64-raw.tar"
docker rm "$cname" >/dev/null

rm -rf "$OUT/sysroot-x86_64" && mkdir -p "$OUT/sysroot-x86_64"
tar -C "$OUT/sysroot-x86_64" -xf "$OUT/sysroot-x86_64-raw.tar" 2>/dev/null || true
rm -f "$OUT/sysroot-x86_64-raw.tar"
mv "$OUT/sysroot-x86_64/packages.txt" "$OUT/sysroot-x86_64.packages.txt"
# Make absolute symlinks inside the sysroot relative so the cross toolchain can follow them.
find "$OUT/sysroot-x86_64" -type l -lname '/*' | while read -r l; do
  t=$(readlink "$l"); rel=$(realpath -m --relative-to="$(dirname "$l")" "$OUT/sysroot-x86_64$t")
  ln -sfn "$rel" "$l"
done

du -sh "$OUT/sysroot-x86_64"
echo "libc6-dev: $(grep '^libc6-dev ' "$OUT/sysroot-x86_64.packages.txt")"
