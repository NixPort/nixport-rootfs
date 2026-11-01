#!/usr/bin/env bash
# 把 build/ 下的组件产物打包成发布资产（build/release/），供 CI 发布到滚动 release
# rootfs-latest。消费方（app release 构建 / Port 开发者 / 手机端）按 spec
# runtime-supply-chain 的契约解包：每个资产都是「单条目」tar.gz，解包后直接得到
# 与本地构建一致的布局。支持部分重建：输入缺失的资产跳过（CI 先取回 release 上
# 现存 meta.json 交给 gen-meta.sh 合并）。
# Packs build/ outputs into release assets for the rolling release rootfs-latest.
# Every asset is a SINGLE-ENTRY tar.gz per the runtime-supply-chain spec. Supports
# partial rebuilds: assets with missing inputs are skipped (CI fetches the current
# meta.json from the release first for gen-meta.sh to merge).
#
# Packs the component outputs under build/ into release assets (build/release/)
# for the CI to publish to the rolling release rootfs-latest. Per the
# runtime-supply-chain spec, every asset is a SINGLE-ENTRY tar.gz: unpacking it
# in the consumer's build/ reproduces the same layout as a local build.
#
# 消费侧解包约定（app 仓库 build-payload.sh 的获取步骤）：
#   rootfs-arm64.tar.gz        → build/rootfs/rootfs-arm64.tar
#   rootfs-x86_64.tar.gz       → build/rootfs/rootfs-x86_64.tar
#   sysroot-arm64.tar.gz       → build/rootfs/sysroot-arm64/
#   sysroot-x86_64.tar.gz      → build/rootfs/sysroot-x86_64/
#   glibc-install.tar.gz       → build/glibc/install/
#   fex-install.tar.gz         → build/fex/install/
#   mesa-install.tar.gz        → build/mesa/install/
#   rootfs-x86-multilib.tar.gz → app 运行时（X86Rootfs.kt）直接 gzip 解包安装
#   rootfs-arm64.tar.zst       → 设备直装包（build-rootfs-image.sh 的锁定树）
#   + rootfs-arm64.uris.txt / .base.txt（锁定清单，nxp-rootfs-topup.sh 消费）
#   每资产一个 .sha256，消费方 sha256sum -c。
#
# Usage: scripts/pack-release-assets.sh   (inputs from build/, outputs to build/release/)
set -euo pipefail
cd "$(dirname "$0")/.."

REL=build/release
rm -rf "$REL" && mkdir -p "$REL"
# 部分重建语义（design D3，nixport-drivers 同款）：输入缺失的资产跳过打包——
# 发布时只替换重建的资产，release 整体仍保持完整（旧资产仍在 release 上）。
# Partial-rebuild semantics (design D3, like nixport-drivers): assets whose
# inputs are missing are skipped — the publish replaces only rebuilt assets and
# the release stays complete (old assets remain on the release).
skip() { echo ">> skip: $1（输入缺失，本次未重建 / input missing, not rebuilt this run）"; }

sha() { # $1 = 已在 $REL 的文件名（生成同名 .sha256）
  ( cd "$REL" && sha256sum "$1" > "$1.sha256" )
}

echo ">> payload 原料：Runtime 树与 x86_64 客机树（tar 文件整体装包）"
# rootfs-arm64.tar 来自 build-rootfs-image.sh（锁定树，canonical）；
# SYSROOT_ONLY 流水线之外本地打包时来自 build-rootfs.sh——两者同布局。
# rootfs-arm64.tar comes from build-rootfs-image.sh (locked tree, canonical);
# outside the pipeline (local packing) it comes from build-rootfs.sh — same layout.
if [[ -f build/rootfs/rootfs-arm64.tar ]]; then
  ( cd build/rootfs && tar --sort=name --owner=0 --group=0 -cf - rootfs-arm64.tar | pigz -9 ) \
    > "$REL/rootfs-arm64.tar.gz"
else
  skip "rootfs-arm64.tar.gz (build-rootfs-image.sh)"
fi
if [[ -f build/rootfs/rootfs-x86_64.tar ]]; then
  ( cd build/rootfs && tar --sort=name --owner=0 --group=0 -cf - rootfs-x86_64.tar | pigz -9 ) \
    > "$REL/rootfs-x86_64.tar.gz"
else
  skip "rootfs-x86_64.tar.gz (build-rootfs.sh)"
fi

echo ">> 交叉编译 sysroot（目录单条目装包，解包即得目录）"
if [[ -d build/rootfs/sysroot-arm64/usr/include ]]; then
  ( cd build/rootfs && tar --sort=name --owner=0 --group=0 -cf - sysroot-arm64 | pigz -9 ) \
    > "$REL/sysroot-arm64.tar.gz"
else
  skip "sysroot-arm64.tar.gz (build-rootfs.sh)"
fi
if [[ -d build/rootfs/sysroot-x86_64/usr/include ]]; then
  ( cd build/rootfs && tar --sort=name --owner=0 --group=0 -cf - sysroot-x86_64 | pigz -9 ) \
    > "$REL/sysroot-x86_64.tar.gz"
else
  skip "sysroot-x86_64.tar.gz (build-x86-sysroot.sh)"
fi

echo ">> 组件安装树（install 单条目装包，解包到 build/<组件>/install/）"
for comp in glibc fex mesa; do
  if [[ -d "build/$comp/install/data/data" ]]; then
    ( cd "build/$comp" && tar --sort=name --owner=0 --group=0 -cf - install | pigz -9 ) \
      > "$REL/$comp-install.tar.gz"
  else
    skip "$comp-install.tar.gz (build-$comp.sh)"
  fi
done

echo ">> x86 multilib 客机树（zst → gz：Android 原生只解 gzip，java.util.zip）"
# gzip 而非 zstd：与 x86-multilib-rootfs.yml 时代的消费方式一致（X86Rootfs.kt）。
# gzip not zstd: matches the consumer since the x86-multilib-rootfs.yml era.
if [[ -f build/rootfs/rootfs-x86-multilib.tar.zst ]]; then
  zstd -dc build/rootfs/rootfs-x86-multilib.tar.zst | pigz -9 > "$REL/rootfs-x86-multilib.tar.gz"
else
  skip "rootfs-x86-multilib.tar.gz (build-rootfs-x86-multilib.sh)"
fi

echo ">> 设备直装包（锁定树的 zst）+ 锁定清单"
if [[ -f build/rootfs/rootfs-arm64.tar ]]; then
  zstd -q -19 -T0 -f build/rootfs/rootfs-arm64.tar -o "$REL/rootfs-arm64.tar.zst"
  for f in rootfs-arm64.uris.txt rootfs-arm64.base.txt rootfs-arm64.packages.txt; do
    [[ -f "build/rootfs/$f" ]] && cp "build/rootfs/$f" "$REL/"
  done
  sha rootfs-arm64.tar.zst
else
  skip "rootfs-arm64.tar.zst (build-rootfs-image.sh)"
fi

echo ">> 逐资产 sha256"
for f in rootfs-arm64.tar.gz rootfs-x86_64.tar.gz sysroot-arm64.tar.gz sysroot-x86_64.tar.gz \
         glibc-install.tar.gz fex-install.tar.gz mesa-install.tar.gz rootfs-x86-multilib.tar.gz; do
  [[ -f "$REL/$f" ]] && sha "$f"
done
[[ -n "$(ls "$REL" 2>/dev/null)" ]] || { echo "nothing packed — check the build outputs" >&2; exit 1; }
ls -la "$REL"
