#!/usr/bin/env bash
# 组装前补齐缺失的组件中间产物（范围修订：组装在供给仓库内完成）。
# 部分重建（rebuild 选择器）时，本地只重建了选择器命中的组件；其余组件的安装树
# 从当前 release 拉取——保证每次组装出的成品 payload 都是「最新组件的完整组合」
# （design D3 co-publish 语义在组装层的延续）。全量重建时什么都不缺，本脚本空跑。
# 顺带取回现存 meta.json（gen-meta.sh 的 BASE_META 合并用）。
#
# Fills in missing component intermediates before assembly (scope revision:
# assembly lives in the supply repo). Under a partial rebuild (rebuild selector)
# only the selected components were rebuilt; the rest are fetched from the
# current release so every assembled payload is a complete combination of the
# latest components. A full rebuild is a no-op. Also fetches the current
# meta.json for gen-meta.sh's BASE_META merge.
#
# 组件中间产物（build/ 内部布局，不进发布资产）：
#   build/glibc/install / build/fex/install / build/mesa/install  ← data/data 入口
#   build/meta.json                                               ← BASE_META
set -euo pipefail
cd "$(dirname "$0")/.."

REPO="${NXP_ROOTFS_REPO:-}"
TAG="${NXP_ROOTFS_TAG:-rootfs-latest}"
[[ -n "$REPO" ]] || REPO="$(git remote get-url origin | sed -E 's#.*[:/]([^/]+/[^/.]+)(\.git)?$#\1#')"
DL=build/release-current
mkdir -p "$DL"

command -v gh >/dev/null || { echo "gh CLI not found" >&2; exit 1; }

# 1) 现存 meta.json（BASE_META；首次发布无 release 时静默跳过）
gh release download "$TAG" -R "$REPO" -p meta.json -O "$DL/meta.json" 2>/dev/null \
  || echo ">> no existing release meta（首次发布?）"

# 2) 缺失的组件安装树逐个从 release 补齐
declare -A ASSET_FOR=( [glibc]=glibc-install.tar.gz [fex]=fex-install.tar.gz [mesa]=mesa-install.tar.gz )
for comp in glibc fex mesa; do
  [[ -d "build/$comp/install/data/data" ]] && { echo ">> $comp install: 本地在（跳过拉取）"; continue; }
  asset="${ASSET_FOR[$comp]}"
  echo ">> $comp install 缺失，从 release 拉取 $asset"
  rm -rf "$DL/$comp"
  mkdir -p "$DL/$comp"
  gh release download "$TAG" -R "$REPO" -p "$asset" -p "$asset.sha256" -D "$DL/$comp" \
    || { echo "COMPONENT FETCH FAIL: $asset missing from $REPO@$TAG — cannot assemble a complete payload" >&2; exit 1; }
  ( cd "$DL/$comp" && sha256sum -c "$asset.sha256" )
  mkdir -p "build/$comp"
  tar -C "build/$comp" -xzf "$DL/$comp/$asset"
  [[ -d "build/$comp/install/data/data" ]] || { echo "FETCH FAIL: $asset did not unpack to build/$comp/install" >&2; exit 1; }
done
echo ">> 组件齐备，可组装"
