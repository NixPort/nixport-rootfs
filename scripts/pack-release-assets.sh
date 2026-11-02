#!/usr/bin/env bash
# 打包本流水线产物为发布资产，按**渠道分目录**落位（多渠道 release 范围修订）：
#   build/release/payload/    成品 payload（app release 消费）
#   build/release/sysroot/    交叉编译 sysroot（Port 开发者）
#   build/release/rootfs/     设备直装包 + 锁定清单
# gen-meta.sh <channel> 逐渠道生成 meta.json。
#
# Packs pipeline outputs into per-CHANNEL release staging dirs (multi-release
# revision): payload/ (app consumer), sysroot/ (Port developers), rootfs/
# (device-direct). gen-meta.sh <channel> then writes each channel's meta.json.
#
# 渠道 → 资产 → 消费方 → 解包约定：
#   payload/  payload-arm64.tar.gz / payload-x86_64.tar.gz → build/payload-rootfs/rootfs-*.tar（app）
#             payload.version / payload.json → build/payload/assets/
#   sysroot/  sysroot-arm64.tar.gz → build/rootfs/sysroot-arm64/（--sysroot 交叉编译）
#   rootfs/   rootfs-arm64.tar.zst + uris/base/packages.txt（设备直装 / topup）
# 每资产一个 .sha256（gen-meta 生成）。
# 单条目装包约定不变：tar.gz 解开后是同名 .tar 文件/目录，消费方布局与本地构建一致。
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/lib/need-artifact.sh

REL=build/release
rm -rf "$REL"
mkdir -p "$REL/payload" "$REL/sysroot"
skip() { echo ">> skip: $1（输入缺失，本渠道本次无产出 / input missing, channel empty this run）"; }
sha() { ( cd "$REL/$2" && sha256sum "$1" > "$1.sha256" ); }

echo ">> payload 渠道：成品 payload"
if [[ -f build/payload-rootfs/rootfs-arm64.tar && -f build/payload-rootfs/rootfs-x86_64.tar ]]; then
  ( cd build/payload-rootfs && tar --sort=name --owner=0 --group=0 -cf - rootfs-arm64.tar | pigz -9 ) \
    > "$REL/payload/payload-arm64.tar.gz"
  ( cd build/payload-rootfs && tar --sort=name --owner=0 --group=0 -cf - rootfs-x86_64.tar | pigz -9 ) \
    > "$REL/payload/payload-x86_64.tar.gz"
  for f in payload.version payload.json; do
    need_artifact "build/payload/assets/$f" scripts/assemble-payload.sh
    cp "build/payload/assets/$f" "$REL/payload/"
  done
else
  skip "payload 渠道（assemble-payload.sh）"
fi

echo ">> sysroot 渠道：交叉编译 sysroot（arm64；x86_64 为内部输入不外发）"
if [[ -d build/rootfs/sysroot-arm64/usr/include ]]; then
  ( cd build/rootfs && tar --sort=name --owner=0 --group=0 -cf - sysroot-arm64 | pigz -9 ) \
    > "$REL/sysroot/sysroot-arm64.tar.gz"
else
  skip "sysroot 渠道（build-rootfs.sh）"
fi

for ch in payload sysroot; do
  [[ -n "$(ls "$REL/$ch" 2>/dev/null)" ]] || { echo "nothing packed for channel $ch" >&2; exit 1; }
  echo ">> 渠道 $ch:"; ls "$REL/$ch"
done
