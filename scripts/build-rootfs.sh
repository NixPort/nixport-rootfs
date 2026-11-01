#!/usr/bin/env bash
# Build the Debian 13 (trixie) arm64 glibc rootfs and the matching cross-compile sysroot.
#
# Outputs (in build/rootfs/):
#   rootfs-arm64.tar            runtime rootfs shipped to the phone (trimmed)
#   sysroot-arm64/              runtime + -dev packages, used as --sysroot for cross builds
#   rootfs-arm64.packages.txt   exact package versions (reproducibility record)
#
# Set SNAPSHOT=YYYYMMDDTHHMMSSZ to pin the Debian archive via snapshot.debian.org.
set -euo pipefail
cd "$(dirname "$0")/.."
# 仓库根的绝对路径：trim 脚本在 cd 进暂存树之后 source，相对路径会解析到树内而找不到。
# Absolute repo root: the trim script is sourced AFTER `cd` into the staged tree,
# so a relative path would resolve inside the tree and miss the file.
ROOT="$PWD"
OUT=build/rootfs
IMAGE="debian:trixie-slim"
# SYSROOT_ONLY=1：跳过 Runtime 树（rootfs-arm64.tar），只产 sysroot-arm64 与
# x86_64 客机树——供给流水线里 Runtime 树由 build-rootfs-image.sh（锁定 uris +
# base digest）产出，本脚本不再重复构建它。
# SYSROOT_ONLY=1: skip the runtime tree (rootfs-arm64.tar) and only produce
# sysroot-arm64 + the x86_64 guest tree — in the supply pipeline the runtime tree
# comes from build-rootfs-image.sh (locked uris + base digest).
SYSROOT_ONLY="${SYSROOT_ONLY:-0}"
mkdir -p "$OUT"
# 前置自检：包清单必须非空（空清单会静默产出空树）。
# Preflight: the package list must exist and be non-empty (an empty list would
# silently produce an empty tree).
if [[ ! -s scripts/lib/rootfs-packages.txt ]]; then
  echo "scripts/lib/rootfs-packages.txt is missing or empty" >&2; exit 1
fi
scripts/ensure-binfmt.sh

APT_SNAPSHOT=""
[[ -n "${SNAPSHOT:-}" ]] && APT_SNAPSHOT="--snapshot $SNAPSHOT"

# Packages present on the phone at runtime. The list has its home in
# scripts/lib/rootfs-packages.txt (the image builder reads it too).
# The list is one name per line; a '#' starts a comment (also at the end of a line).
RUNTIME_PKGS=()
while read -r pkg; do
  pkg="${pkg%%#*}"
  pkg="${pkg//[[:space:]]/}"
  [[ -n "$pkg" ]] && RUNTIME_PKGS+=("$pkg")
done < scripts/lib/rootfs-packages.txt

# Extra packages only needed to cross-compile FEX/Mesa against this sysroot.
DEV_PKGS=(
  libc6-dev libstdc++-14-dev libzstd-dev zlib1g-dev libexpat1-dev libdrm-dev
  libx11-dev libx11-xcb-dev libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-shm0-dev
  libxcb-sync-dev libxcb-xfixes0-dev libxcb-randr0-dev libxshmfence-dev libxrandr-dev
  libwayland-dev libvulkan-dev libudev-dev
)

build_tree() { # $1 = name, $2 = docker platform, rest = packages
  local name=$1 platform=$2; shift 2
  local cname="fexdroid-$name-$$"
  docker rm -f "$cname" >/dev/null 2>&1 || true
  docker run --platform "$platform" --name "$cname" "$IMAGE" bash -euc "
    export DEBIAN_FRONTEND=noninteractive
    apt-get update $APT_SNAPSHOT -qq
    apt-get install -y -qq --no-install-recommends $* >/dev/null
    apt-get clean
    rm -rf /var/lib/apt/lists/*
    # Private library dirs: Debian resolves e.g. libpulsecommon-17.0.so (in
    # /usr/lib/<multiarch>/pulseaudio/) through its own ld.so.cache, but
    # ld.so.conf.d never lists the private dirs and the phone runtime has no
    # distro ldconfig to refresh the cache. Register them and rebuild the cache
    # here, at build time (MHGU port, my-docs/移植案例/MHGU移植专栏/移植MHGU-真机联调踩坑实录.md §5.1).
    multiarch=\$(dpkg --print-architecture | sed 's/^arm64$/aarch64-linux-gnu/; s/^amd64$/x86_64-linux-gnu/')
    for d in "/usr/lib/\$multiarch/pulseaudio"; do
      [ -d "\$d" ] && printf '%s\n' "\$d" >> /etc/ld.so.conf.d/nixport-private-libs.conf
    done
    [ ! -f /etc/ld.so.conf.d/nixport-private-libs.conf ] || ldconfig
    dpkg-query -W -f='\${Package} \${Version}\n' > /packages.txt
  "
  docker export "$cname" > "$OUT/$name.tar"
  docker rm "$cname" >/dev/null
}

if [[ "${ONLY_X86:-0}" == 1 ]]; then SKIP_ARM=1; fi
if [[ "${SKIP_ARM:-0}" != 1 ]]; then
if [[ "$SYSROOT_ONLY" != 1 ]]; then
echo ">> runtime rootfs"
build_tree rootfs-arm64-raw linux/arm64 "${RUNTIME_PKGS[@]}"
fi
echo ">> sysroot"
build_tree sysroot-arm64-raw linux/arm64 "${RUNTIME_PKGS[@]}" "${DEV_PKGS[@]}"

if [[ "$SYSROOT_ONLY" != 1 ]]; then
# 把 runtime 树解到暂存区再裁剪。解包必须先于裁剪：裁剪脚本在暂存区内就地删除文件，
# 没有这棵树就没有可裁剪的对象；raw tar 由上面的 build_tree 导出。
# Unpack the runtime tree into the staging area before trimming. The trim rules
# delete files in place inside the staged tree, so the tree must exist first;
# the raw tar comes from the build_tree call above.
rm -rf "$OUT/stage" && mkdir -p "$OUT/stage"
tar -C "$OUT/stage" -xf "$OUT/rootfs-arm64-raw.tar" 2>/dev/null || true
# 解包自检：暂存树必须解出 usr/bin（raw tar 损坏时在此 fail fast，而不是在裁剪里
# 报无关错误）。
# Stage sanity: the staged tree must contain usr/bin (a corrupt raw tar fails
# here instead of with an unrelated error inside trim).
[[ -d "$OUT/stage/usr/bin" ]] || { echo "stage unpack failed: $OUT/stage/usr/bin missing (raw tar corrupt?)"; exit 1; }

echo ">> trimming"
# Trim the runtime rootfs: docs, man pages, locales, container leftovers.
# The rules live in scripts/lib/rootfs-trim.sh (the image builder trims the same way).
# Absolute path: sourced after `cd` into the stage, a relative path misses the file.
( cd "$OUT/stage"; source "$ROOT/scripts/lib/rootfs-trim.sh" )
mv "$OUT/stage/packages.txt" "$OUT/rootfs-arm64.packages.txt"
# reference client, part of the Runtime so every Port can call the host.
install -D -m 0755 scripts/nxp-port "$OUT/stage/usr/local/bin/nxp-port"
echo ">> packing arm64 runtime tree"
tar -C "$OUT/stage" --numeric-owner --owner=0 --group=0 -cf "$OUT/rootfs-arm64.tar" .
rm -rf "$OUT/stage" "$OUT/rootfs-arm64-raw.tar"
echo "   arm64 runtime tree: $(du -h "$OUT/rootfs-arm64.tar" | cut -f1)"
fi # SYSROOT_ONLY

rm -rf "$OUT/sysroot-arm64" && mkdir -p "$OUT/sysroot-arm64"
tar -C "$OUT/sysroot-arm64" -xf "$OUT/sysroot-arm64-raw.tar" 2>/dev/null || true
rm -f "$OUT/sysroot-arm64-raw.tar"
# Make absolute symlinks inside the sysroot relative so the cross toolchain can follow them.
find "$OUT/sysroot-arm64" -type l -lname '/*' | while read -r l; do
  t=$(readlink "$l"); rel=$(realpath -m --relative-to="$(dirname "$l")" "$OUT/sysroot-arm64$t")
  ln -sfn "$rel" "$l"
done
echo "   arm64 sysroot: $(du -sh "$OUT/sysroot-arm64" | cut -f1)"

fi # SKIP_ARM

# x86_64 guest rootfs for FEX (phase 2). Grows with phase 3 (vulkan-tools); the multilib
# (amd64 + i386) tree is built separately by scripts/build-rootfs-x86-multilib.sh.
X86_PKGS=(libc6 libstdc++6 coreutils libx11-6 libxcb1 libvulkan1 vulkan-tools  # vulkan-tools: vkcube for phase 3
          libpulse0 pulseaudio-utils)                                          # phase 4: x86 audio clients
echo ">> x86_64 guest rootfs"
build_tree rootfs-x86_64-raw linux/amd64 "${X86_PKGS[@]}"
rm -rf "$OUT/stage" && mkdir -p "$OUT/stage"
tar -C "$OUT/stage" -xf "$OUT/rootfs-x86_64-raw.tar" 2>/dev/null || true
echo ">> trimming"
( cd "$OUT/stage"
  rm -rf usr/share/doc/* usr/share/man/* usr/share/info/* usr/share/lintian var/log/* .dockerenv
  find usr/share/locale -mindepth 1 -maxdepth 1 ! -name 'locale.alias' -exec rm -rf {} + 2>/dev/null || true
)
mv "$OUT/stage/packages.txt" "$OUT/rootfs-x86_64.packages.txt"
echo ">> packing x86_64 guest tree"
tar -C "$OUT/stage" --numeric-owner --owner=0 --group=0 -cf "$OUT/rootfs-x86_64.tar" .
rm -rf "$OUT/stage" "$OUT/rootfs-x86_64-raw.tar"
echo "   x86_64 guest tree: $(du -h "$OUT/rootfs-x86_64.tar" | cut -f1)"

if [[ "$SYSROOT_ONLY" != 1 && "${SKIP_ARM:-0}" != 1 ]]; then
  du -sh "$OUT/rootfs-arm64.tar"
fi
if [[ "${SKIP_ARM:-0}" != 1 ]]; then
  du -sh "$OUT/sysroot-arm64"
fi
du -sh "$OUT/rootfs-x86_64.tar"
if [[ "$SYSROOT_ONLY" != 1 && "${SKIP_ARM:-0}" != 1 ]]; then
  echo "glibc: $(grep '^libc6 ' "$OUT/rootfs-arm64.packages.txt")"
fi
