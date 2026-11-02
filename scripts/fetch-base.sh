#!/usr/bin/env bash
# 从 base-latest 拉取原料资产到 build/rootfs/（与本地构建同形布局）。
# 资产名单作参数；逐资产 sha256 校验；缺失/校验失败立即失败并指名资产与渠道——
# **没有本地重建回退**（design D5）：原料缺了就去触发 base.yml。
#
# Fetches raw-material assets from the base-latest release into build/rootfs/
# (same layout as a local build). Per-asset sha256 verified. NO local rebuild
# fallback — the remedy is to run the base.yml workflow.
#
# 用法 / usage:
#   fetch-base.sh <asset>...        # 如 sysroot-arm64.tar.gz；或 rootfs-arm64.tar.gz
#
# env: NXP_ROOTFS_REPO（默认本仓库 origin）、NXP_BASE_TAG（默认 base-latest）
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${NXP_BASE_TAG:-base-latest}"
REPO="${NXP_ROOTFS_REPO:-}"
[[ -n "$REPO" ]] || REPO="$(git remote get-url origin | sed -E 's#.*[:/]([^/]+/[^/.]+)(\.git)?$#\1#')"
[[ $# -ge 1 ]] || { echo "usage: fetch-base.sh <asset.tar.gz|asset.txt>..." >&2; exit 1; }

fail() { echo "FETCH FAIL: $*" >&2; exit 1; }
command -v gh >/dev/null || fail "gh CLI not found"

DL=build/base-dl
mkdir -p "$DL" build/rootfs
PATTERNS=""
for a in "$@"; do PATTERNS="$PATTERNS -p $a -p $a.sha256"; done
echo ">> 渠道 ${REPO}@${TAG}：拉取原料 $*"
gh release download "$TAG" -R "$REPO" $PATTERNS -D "$DL" \
  || fail "release $TAG not reachable at $REPO — trigger the base.yml workflow first (Actions → base assets → Run workflow)"

cd "$DL"
shopt -s nullglob
for f in ./*.sha256; do sha256sum -c "$f" || fail "sha256 mismatch: $f"; done
cd - >/dev/null

for a in "$@"; do
  [[ -f "$DL/$a" ]] || fail "required asset missing on the channel: $a"
  case "$a" in
    *.tar.gz) tar -C build/rootfs -xzf "$DL/$a" ;;
    *) cp "$DL/$a" build/rootfs/ ;;
  esac
done
rm -rf "$DL"
echo ">> 原料就绪：build/rootfs/（来源 ${REPO}@${TAG}）"
