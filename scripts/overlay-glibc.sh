#!/usr/bin/env bash
# 把补丁版 glibc 覆盖进 Runtime 树 tar（base.yml 专用——extract-rootfs-repo 拆分后
# glibc 归位原料层：树 = 最终形态）。覆盖后原料树与 payload 的树内容一致，树自带 seccomp 规避/路径重定向
# （NOTES.md N-002/N-009/N-017）。（历史：rootfs-latest 直装渠道已退役。）
#
# sysroot-arm64 刻意不走这里：它由 SYSROOT_ONLY 的独立 docker 树裁剪而来
# （build-rootfs.sh），保持纯 Debian——交叉编译视图不被补丁污染（ABI 一致，但
# "干净上游"语义更对）。别人拿 sysroot 交叉编译的行为不受本次迁移影响。
#
# Overlay the patched glibc into the runtime tree tar (base.yml only): the tree
# becomes the final artifact, so the base tree and the payload tree agree. sysroot-arm64
# deliberately stays stock Debian — the cross-compilation view is unaffected.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh
source scripts/lib/need-artifact.sh
need_artifact build/rootfs/rootfs-arm64.tar scripts/build-rootfs-image.sh
need_artifact "build/glibc/install$FXD_LDSO" scripts/build-glibc.sh

STAGE=build/rootfs/overlay-stage
rm -rf "$STAGE" && mkdir -p "$STAGE"
tar -C "$STAGE" -xf build/rootfs/rootfs-arm64.tar
# DESTDIR 前缀剥离：install$FXD_ROOT 的内容即树根（与 assemble-payload 同一约定）。
# Strip the DESTDIR prefix: install$FXD_ROOT's contents map onto the tree root
# (same convention as assemble-payload).
cp -a "build/glibc/install$FXD_ROOT/." "$STAGE/"
tar -C "$STAGE" --numeric-owner --owner=0 --group=0 --sort=name \
    -cf build/rootfs/rootfs-arm64.tar .
rm -rf "$STAGE"
echo ">> patched glibc overlaid into rootfs-arm64.tar"
