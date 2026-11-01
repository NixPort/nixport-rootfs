#!/usr/bin/env bash
# Trim the runtime rootfs: docs, man pages, locales, container leftovers.
# Sourced inside the staged tree (cwd = the tree), by scripts/build-rootfs.sh and
# scripts/build-rootfs-image.sh - the two builders stage the same tree, so the
# rules live in one place.
#
# 注意：本脚本在暂存树内就地裁剪，`cd` 之后的相对路径会解析到树里 —— 调用方必须用
# 仓库根的绝对路径 source 本文件。
# NOTE: this script trims the staged tree in place; after the caller's `cd`, a
# relative path would resolve inside the tree - callers MUST source it by the
# absolute path from the repo root.

rm -rf usr/share/doc/* usr/share/man/* usr/share/info/* usr/share/lintian \
       var/cache/debconf/*-old var/log/* .dockerenv
find usr/share/locale -mindepth 1 -maxdepth 1 ! -name 'locale.alias' -exec rm -rf {} + 2>/dev/null || true
# Vulkan: Turnip (our build, scripts/build-mesa.sh) on Adreno; lavapipe (LLVM, from
# Debian's mesa-vulkan-drivers) as the software fallback on phones without KGSL
# (Mali, Xclipse, PowerVR; NOTES.md N-027). Every other Debian Vulkan driver targets
# desktop/DRM GPUs that an Android app can never open: drop them and their manifests.
for icd in usr/share/vulkan/icd.d/*.json; do
  case "$icd" in */lvp_icd.*) ;; *) rm -f "$icd" ;; esac
done
# 目录不存在时 find 返回非零；`|| true` 免得它配合调用方的 set -e 中止整次构建。
# find exits non-zero when the directory is absent; `|| true` keeps that from
# aborting the whole build under the caller's set -e.
find usr/lib/aarch64-linux-gnu -maxdepth 1 -name 'libvulkan_*.so' ! -name 'libvulkan_lvp.so' -delete 2>/dev/null || true
# Mesa's layers: device_select is implicit (loaded into every Vulkan app, Turnip included)
# and only reorders GPUs; the overlay is a desktop HUD. Neither is wanted here.
rm -f usr/share/vulkan/implicit_layer.d/VkLayer_MESA_device_select.json \
      usr/share/vulkan/explicit_layer.d/VkLayer_MESA_overlay.json \
      usr/lib/aarch64-linux-gnu/libVkLayer_MESA_*.so usr/bin/mesa-overlay-control.py
# GL drivers: Xvfb only needs swrast to bring up its GLX extension (software GLX,
# clients render with their own Mesa through drisw). Guest clients assert without a
# GLX visual (NOTES.md N-028). Every hardware DRI driver goes: no DRM devices here.
find usr/lib/aarch64-linux-gnu/dri -mindepth 1 ! -name swrast_dri.so ! -name libdril_dri.so -delete 2>/dev/null || true
