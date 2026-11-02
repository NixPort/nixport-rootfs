#!/usr/bin/env bash
# 从 x86-latest release 拉取 x86_64 侧资产（客机树 + thunks sysroot），落位到
# build/rootfs/ 的本地构建布局（范围修订：x86 构建剥离至 x86.yml，主管线只消费）。
# 逐资产 sha256 校验；缺失/校验失败立即失败并指名资产与渠道——**没有本地重建回退**
# （缺了就该去触发 x86.yml，而不是把主管线拖回慢速全量构建）。
#
# Fetches the x86-side assets (guest tree + thunks sysroot) from the x86-latest
# release into build/rootfs/ (same layout as a local build). Per-asset sha256
# verified; any failure fails immediately naming the asset and the channel —
# NO local rebuild fallback (the remedy is to run the x86.yml workflow).
#
# 环境变量 / env: NXP_ROOTFS_REPO（默认本仓库 origin）
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${NXP_X86_TAG:-x86-latest}"
REPO="${NXP_ROOTFS_REPO:-}"
[[ -n "$REPO" ]] || REPO="$(git remote get-url origin | sed -E 's#.*[:/]([^/]+/[^/.]+)(\.git)?$#\1#')"
DL=build/x86-dl
fail() { echo "FETCH FAIL: $*" >&2; exit 1; }

command -v gh >/dev/null || fail "gh CLI not found"
mkdir -p "$DL" build/rootfs
echo ">> 渠道 ${REPO}@${TAG}：x86_64 客机树 + sysroot"
gh release download "$TAG" -R "$REPO" -p rootfs-x86_64.tar.gz -p rootfs-x86_64.tar.gz.sha256 \
  -p sysroot-x86_64.tar.gz -p sysroot-x86_64.tar.gz.sha256 -D "$DL" \
  || fail "release $TAG not reachable at $REPO — trigger the x86.yml workflow first (Actions → x86 assets → Run workflow)"

cd "$DL"
shopt -s nullglob
for f in ./*.sha256; do sha256sum -c "$f" || fail "sha256 mismatch: $f"; done
cd - >/dev/null

tar -C build/rootfs -xzf "$DL/rootfs-x86_64.tar.gz"        # → rootfs-x86_64.tar
tar -C build/rootfs -xzf "$DL/sysroot-x86_64.tar.gz"       # → sysroot-x86_64/
[[ -f build/rootfs/rootfs-x86_64.tar ]] || fail "rootfs-x86_64.tar.gz did not unpack to rootfs-x86_64.tar"
[[ -d build/rootfs/sysroot-x86_64/usr/include ]] || fail "sysroot-x86_64.tar.gz did not unpack to sysroot-x86_64/"
rm -rf "$DL"
echo ">> x86_64 资产就绪（来源 ${REPO}@${TAG}）"
