#!/usr/bin/env bash
# 从上游拉 uruntime（AppImage 静态 runtime），校验 sha256 后落在 build/uruntime/uruntime，
# 供 build-rootfs-image.sh / build-rootfs.sh 装进 rootfs 的 /usr/local/bin/。
#
# uruntime 不是 Debian 包（装不进包表），是单独的拉取 + pin：版本与校验和在
# scripts/uruntime-pin.env 里，升级 = 改 pin + 重验。
#
# Fetches uruntime (the static AppImage runtime) from upstream, verifies its
# sha256 and places it at build/uruntime/uruntime for the rootfs builders to
# install into /usr/local/bin/. Not a Debian package, so it is a separate
# pinned fetch: version and checksum live in scripts/uruntime-pin.env.
set -euo pipefail
cd "$(dirname "$0")/.."

# shellcheck source=scripts/uruntime-pin.env
source scripts/uruntime-pin.env

fail() { echo "FETCH FAIL: $*" >&2; exit 1; }
command -v gh >/dev/null || fail "gh CLI not found"
[[ -n "$URUNTIME_VERSION" ]] || fail "URUNTIME_VERSION not set in scripts/uruntime-pin.env"
[[ -n "$URUNTIME_ASSET" ]] || fail "URUNTIME_ASSET not set in scripts/uruntime-pin.env"
[[ -n "$URUNTIME_SHA256" ]] || fail "URUNTIME_SHA256 not set in scripts/uruntime-pin.env"

REPO="${URUNTIME_REPO:-VHSgunzo/uruntime}"
DL=build/uruntime-dl
mkdir -p "$DL"
echo ">> 上游 ${REPO}@${URUNTIME_VERSION}：uruntime $URUNTIME_ASSET"
gh release download "$URUNTIME_VERSION" -R "$REPO" -p "$URUNTIME_ASSET" -D "$DL" \
  || fail "asset $URUNTIME_ASSET not found on $REPO@$URUNTIME_VERSION"

# sha256 重验：pin 是构建契约，资产被换掉就在这里停住（不靠文件名信任上游）。
# Re-verify the checksum: the pin is the build contract; a swapped asset stops
# here (upstream filenames are not trusted).
sha256_of() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}
GOT="$(sha256_of "$DL/$URUNTIME_ASSET")"
[[ "$GOT" == "$URUNTIME_SHA256" ]] \
  || fail "sha256 mismatch for $URUNTIME_ASSET: got $GOT, pinned $URUNTIME_SHA256"

# 结构自检：必须是 aarch64 ELF（装错架构的 runtime 到手机上才炸不如现在就停）。
# Structural check: it must be an aarch64 ELF (a wrong-architecture runtime
# would only blow up on the phone; stop here instead).
file "$DL/$URUNTIME_ASSET" | grep -q "ARM aarch64" || fail "uruntime asset is not aarch64"

mkdir -p build/uruntime
install -m 0755 "$DL/$URUNTIME_ASSET" build/uruntime/uruntime
rm -rf "$DL"
echo ">> uruntime 就绪：build/uruntime/uruntime（${REPO}@${URUNTIME_VERSION} ${URUNTIME_ASSET}）"
