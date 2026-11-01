#!/usr/bin/env bash
# Build the guest rootfs as a pinned, reproducible artifact (the DroidDeck way).
#
# scripts/build-rootfs.sh builds the same tree, but two things move under the
# build: the base image is taken by tag, and apt resolves the packages against
# whatever the archive holds that day. The result is a tree nobody can rebuild
# byte for byte, and it is shipped inside the APK (hundreds of megabytes).
#
# DroidDeck solves both differently (tools/linuxfs/debian-image.env): the base
# is a whole image pinned by sha256 and published as a release asset, and the
# app fetches that asset at runtime instead of carrying it.
#
# This script is the same build with the two things an artifact needs:
#   - the base image is recorded by digest (rootfs-arm64.base.txt says what the
#     tree is made of),
#   - the packages are locked: apt resolves them once, the exact URLs with their
#     checksums are written out (rootfs-arm64.uris.txt), and the tree is then
#     built from exactly those bytes. A later build repeats it, and a phone can
#     top up its rootfs from the same list (scripts/nxp-rootfs-topup.sh).
#
# SUITE picks the Debian release the tree is built from:
#   SUITE=trixie  (default) Debian 13: glibc 2.41 - the stable base, and what
#                             the Port packaging ceiling assumes (NXP_MAX_GLIBC)
#   SUITE=sid                Debian sid: glibc 2.43, libstdc++ 16
#                             - the rolling base; pin it with SNAPSHOT.
#
# Set SNAPSHOT=YYYYMMDDTHHMMSSZ to pin the Debian archive via snapshot.debian.org.
#
# Outputs (in build/rootfs/):
#   rootfs-arm64.tar            the tree (the same layout build-rootfs.sh leaves)
#   rootfs-arm64.packages.txt   the installed versions
#   rootfs-arm64.uris.txt       the lock: package URLs and checksums
#   rootfs-arm64.base.txt       the base image digest
set -euo pipefail
cd "$(dirname "$0")/.."
# 仓库根的绝对路径：trim 脚本在 cd 进暂存树之后 source，相对路径会解析到树内而找不到。
# Absolute repo root: the trim script is sourced AFTER `cd` into the staged tree,
# so a relative path would resolve inside the tree and miss the file.
ROOT="$PWD"
OUT=build/rootfs
SUITE="${SUITE:-trixie}"
IMAGE="${IMAGE:-debian:$SUITE-slim}"
PLATFORM="${PLATFORM:-linux/arm64}"
mkdir -p "$OUT"
# scripts/config.sh holds the on-device path contract (FXD_ROOT/FXD_LDSO);
# the patched glibc version is internalized in build-glibc.sh (from packages.txt).
# shellcheck source=scripts/config.sh
source scripts/config.sh

# The package list has its home in scripts/lib/rootfs-packages.txt.
# The list is one name per line; a '#' starts a comment (also at the end of a line).
RUNTIME_PKGS=()
while read -r pkg; do
  pkg="${pkg%%#*}"
  pkg="${pkg//[[:space:]]/}"
  [[ -n "$pkg" ]] && RUNTIME_PKGS+=("$pkg")
done < scripts/lib/rootfs-packages.txt
if [[ ${#RUNTIME_PKGS[@]} -eq 0 ]]; then
  echo "scripts/lib/rootfs-packages.txt is empty" >&2
  exit 1
fi

APT_SNAPSHOT=""
[[ -n "${SNAPSHOT:-}" ]] && APT_SNAPSHOT="--snapshot $SNAPSHOT"

cname="fexdroid-rootfs-image-$$"
docker rm -f "$cname" >/dev/null 2>&1 || true

# The base's glibc does not survive the payload build: scripts/build-payload.sh
# replaces it with OUR patched glibc (scripts/build-glibc.sh, GLIBC_DEB_VERSION).
# So the base decides the *other* libraries (libstdc++ above all), and our
# glibc has to be new enough for them - a newer base with our old glibc would
# leave packages that want symbols our loader does not have.
# 契约内化后本脚本不再持有 glibc 版本常量：补丁版 glibc 跟随 trixie 树的 libc6
# （build-glibc.sh 从 packages.txt 读取）。非 trixie 基底的包可能需要更新的 glibc，
# 发布前对照 packages.txt 与 meta.json 的 libc6，必要时重建 glibc。
# After contract internalization this script holds no glibc version constant: the
# patched glibc tracks the trixie tree's libc6 (build-glibc.sh reads packages.txt).
# A non-trixie base may carry packages needing a newer glibc — compare packages.txt
# against meta.json's libc6 and rebuild glibc if needed before shipping.
if [[ "$SUITE" != "trixie" ]]; then
  echo "::warning::SUITE=$SUITE: this base's packages may need a newer glibc than the patched build provides"
  echo "::warning::(the patched glibc tracks the trixie tree's libc6 via rootfs-arm64.packages.txt;"
  echo "::warning::compare and rebuild scripts/build-glibc.sh if this base is newer)."
fi

# binfmt 自保证（extract-rootfs-repo 首轮 CI 教训）：不能信任 CI 的 setup-qemu
# action（tonistiigi/binfmt:latest 兼容性漂移会让 arm64 容器 exec format error），
# 与 build-rootfs.sh 一致——脚本自己注册并用真实容器验证。
# binfmt self-provision (first CI round lesson): never trust the CI setup-qemu
# action alone (tonistiigi/binfmt:latest drift breaks arm64 exec); like
# build-rootfs.sh, register and verify with a real container here.
scripts/ensure-binfmt.sh
echo ">> base image: $IMAGE ($PLATFORM), suite $SUITE"
docker pull --platform "$PLATFORM" "$IMAGE" >/dev/null
# The digest is the identity of the base: the same tree must come from the same
# image, so the record keeps it.
BASE_DIGEST="$(docker image inspect "$IMAGE" \
  --format '{{if .RepoDigests}}{{index .RepoDigests 0}}{{else}}{{.Id}}{{end}}')"

echo ">> building the tree"
# The records are written to the mounted output directory, not inside the
# container: they are part of the build's result, not of the tree.
docker run --platform "$PLATFORM" --name "$cname" -v "$PWD/$OUT:/out" "$IMAGE" bash -euc "
  export DEBIAN_FRONTEND=noninteractive
  apt-get update $APT_SNAPSHOT -qq
  # The lock: apt resolves the packages (dependencies included) and prints the
  # exact URLs with their checksums. Nothing is downloaded yet.
  apt-get install -y --no-install-recommends --print-uris ${RUNTIME_PKGS[*]} > /out/rootfs-arm64.uris.txt
  # Then the same set for real - from the lock, so the tree matches the record.
  apt-get install -y --no-install-recommends \$(grep '^http' /out/rootfs-arm64.uris.txt | cut -d\"'\" -f1)
  apt-get clean
  rm -rf /var/lib/apt/lists/*
  # Private library dirs: Debian resolves e.g. libpulsecommon-17.0.so (in
  # /usr/lib/<multiarch>/pulseaudio/) through its own ld.so.cache, but
  # ld.so.conf.d never lists the private dirs and the phone runtime has no
  # distro ldconfig to refresh the cache. Register them and rebuild the cache
  # here, at build time (MHGU port, my-docs/移植案例/MHGU移植专栏/移植MHGU-真机联调踩坑实录.md §5.1).
  multiarch=\$(dpkg --print-architecture | sed 's/^arm64\$/aarch64-linux-gnu/; s/^amd64\$/x86_64-linux-gnu/')
  for d in \"/usr/lib/\$multiarch/pulseaudio\"; do
    [ -d \"\$d\" ] && printf '%s\n' \"\$d\" >> /etc/ld.so.conf.d/nixport-private-libs.conf
  done
  [ ! -f /etc/ld.so.conf.d/nixport-private-libs.conf ] || ldconfig
  dpkg-query -W -f='\${Package} \${Version}\n' > /out/rootfs-arm64.packages.txt
"
docker export "$cname" > "$OUT/rootfs-arm64-raw.tar"
docker rm "$cname" >/dev/null
echo "$BASE_DIGEST" > "$OUT/rootfs-arm64.base.txt"

echo ">> staging"
rm -rf "$OUT/stage" && mkdir -p "$OUT/stage"
tar -C "$OUT/stage" -xf "$OUT/rootfs-arm64-raw.tar"

echo ">> trimming"
# Absolute path: sourced after `cd` into the stage, a relative path misses the file.
( cd "$OUT/stage"; source "$ROOT/scripts/lib/rootfs-trim.sh" )

# reference client, part of the Runtime so every Port can call the host.
install -D -m 0755 scripts/nxp-port "$OUT/stage/usr/local/bin/nxp-port"

echo ">> packing"
tar -C "$OUT/stage" --numeric-owner --owner=0 --group=0 -cf "$OUT/rootfs-arm64.tar" .
rm -rf "$OUT/stage" "$OUT/rootfs-arm64-raw.tar"
echo "   arm64 runtime tree: $(du -h "$OUT/rootfs-arm64.tar" | cut -f1)"

du -sh "$OUT/rootfs-arm64.tar"
echo "glibc: $(grep '^libc6 ' "$OUT/rootfs-arm64.packages.txt" || echo unknown)"
echo "base: $(cat "$OUT/rootfs-arm64.base.txt")"
echo "lock: $(grep -c '^http' "$OUT/rootfs-arm64.uris.txt" || echo 0) packages"
