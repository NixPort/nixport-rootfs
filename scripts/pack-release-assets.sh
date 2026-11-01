#!/usr/bin/env bash
# 把本流水线产物打包成发布资产（build/release/），供 CI 发布到滚动 release
# rootfs-latest。范围修订（extract-rootfs-repo）后的资产集 = 最终交付物：
# 成品 payload（payload-*.tar.gz + payload.version/json）、交叉 sysroot、
# x86 multilib（独立流水线重建，本脚本跳过缺席）、设备直装 zst + 锁定清单。
# 组件中间产物（裸 Debian 树、组件安装树）不再外发——它们只是组装的内部输入。
#
# Packs pipeline outputs into release assets for the rolling release rootfs-latest.
# Asset set after the scope revision = final deliverables: assembled payload tars
# (+ version/json), cross sysroots, x86 multilib (built by the separate multilib
# workflow; skipped when absent), device-direct zst + lock lists. Component
# intermediates (raw trees, install trees) are internal assembly inputs only.
#
# 消费侧解包约定（单条目 tar.gz，解包即得本地构建同形布局）：
#   payload-arm64.tar.gz        → build/payload-rootfs/rootfs-arm64.tar（app 打包）
#   payload-x86_64.tar.gz       → build/payload-rootfs/rootfs-x86_64.tar
#   payload.version|payload.json → build/payload/assets/（随 APK assets 分发）
#   sysroot-arm64.tar.gz        → build/rootfs/sysroot-arm64/（Port 开发者交叉编译）
#   rootfs-x86-multilib.tar.gz  → app 运行时（X86Rootfs.kt）直接解压安装
#   rootfs-arm64.tar.zst        → 设备直装（build-rootfs-image.sh 锁定树）
#   + uris/base 锁定清单（nxp-rootfs-topup.sh 消费）
# 每资产一个 .sha256，消费方 sha256sum -c。
set -euo pipefail
cd "$(dirname "$0")/.."

REL=build/release
rm -rf "$REL" && mkdir -p "$REL"
# 部分重建语义（design D3，nixport-drivers 同款）：输入缺失的资产跳过打包——
# 发布时只替换重建的资产，release 整体仍保持完整（旧资产仍在 release 上，
# gen-meta.sh 以 BASE_META 合并）。
# Partial-rebuild semantics (design D3, like nixport-drivers): assets whose
# inputs are missing are skipped — the publish replaces only rebuilt assets and
# the release stays complete (old assets remain, merged via BASE_META).
skip() { echo ">> skip: $1（输入缺失，本次未重建 / input missing, not rebuilt this run）"; }
sha() { ( cd "$REL" && sha256sum "$1" > "$1.sha256" ); }

echo ">> 成品 payload（assemble-payload.sh 的组装结果）"
if [[ -f build/payload-rootfs/rootfs-arm64.tar && -f build/payload-rootfs/rootfs-x86_64.tar ]]; then
  ( cd build/payload-rootfs && tar --sort=name --owner=0 --group=0 -cf - rootfs-arm64.tar | pigz -9 ) \
    > "$REL/payload-arm64.tar.gz"
  ( cd build/payload-rootfs && tar --sort=name --owner=0 --group=0 -cf - rootfs-x86_64.tar | pigz -9 ) \
    > "$REL/payload-x86_64.tar.gz"
  sha payload-arm64.tar.gz
  sha payload-x86_64.tar.gz
else
  skip "payload-*.tar.gz (scripts/assemble-payload.sh)"
fi
for f in payload.version payload.json; do
  [[ -f "build/payload/assets/$f" ]] && cp "build/payload/assets/$f" "$REL/" && sha "$f"
done

echo ">> 交叉编译 sysroot（目录单条目装包，解包即得目录）"
if [[ -d build/rootfs/sysroot-arm64/usr/include ]]; then
  ( cd build/rootfs && tar --sort=name --owner=0 --group=0 -cf - sysroot-arm64 | pigz -9 ) \
    > "$REL/sysroot-arm64.tar.gz"
  sha sysroot-arm64.tar.gz
else
  skip "sysroot-arm64.tar.gz (build-rootfs.sh)"
fi
if [[ -d build/rootfs/sysroot-x86_64/usr/include ]]; then
  ( cd build/rootfs && tar --sort=name --owner=0 --group=0 -cf - sysroot-x86_64 | pigz -9 ) \
    > "$REL/sysroot-x86_64.tar.gz"
  sha sysroot-x86_64.tar.gz
else
  skip "sysroot-x86_64.tar.gz (build-x86-sysroot.sh)"
fi

echo ">> x86 multilib 客机树（独立流水线重建；缺席则跳过）"
if [[ -f build/rootfs/rootfs-x86-multilib.tar.zst ]]; then
  # gzip 而非 zstd：与 X86Rootfs.kt 的消费方式一致（Android 原生只解 gzip）。
  zstd -dc build/rootfs/rootfs-x86-multilib.tar.zst | pigz -9 > "$REL/rootfs-x86-multilib.tar.gz"
  sha rootfs-x86-multilib.tar.gz
else
  skip "rootfs-x86-multilib.tar.gz (multilib workflow)"
fi

echo ">> 设备直装包（锁定树的 zst）+ 锁定清单"
if [[ -f build/rootfs/rootfs-arm64.tar ]]; then
  zstd -q -19 -T0 -f build/rootfs/rootfs-arm64.tar -o "$REL/rootfs-arm64.tar.zst"
  for f in rootfs-arm64.uris.txt rootfs-arm64.base.txt rootfs-arm64.packages.txt; do
    [[ -f "build/rootfs/$f" ]] && cp "build/rootfs/$f" "$REL/" && sha "$f"
  done
  sha rootfs-arm64.tar.zst
else
  skip "rootfs-arm64.tar.zst (build-rootfs-image.sh)"
fi

[[ -n "$(ls "$REL" 2>/dev/null)" ]] || { echo "nothing packed — check the build outputs" >&2; exit 1; }
ls -la "$REL"
