#!/usr/bin/env bash
# 发布前存活自检（spec runtime-supply-chain：发布前自检；design D7）。
# 任一检查失败即退出非零——CI 的发布步骤因此 MUST NOT 执行，渠道保持上一次
# 可用资产集。
#
# Pre-publish liveness check (spec runtime-supply-chain; design D7). Any failure
# exits non-zero so the CI publish step MUST NOT run and the channel keeps the
# last good asset set.
#
# 检查三件事：
#   1. Runtime 树在 arm64 容器内以设备路径挂载后可执行基础程序，且 ldconfig
#      能在设备路径重建缓存（build-payload.sh 同款手法）；
#   2. 组件安装树包含 meta 声明的关键产物（loader、FEX 解释器、Turnip ICD）；
#   3. 其余资产结构完整（sysroot 头文件、multilib 压缩包可读）。
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh

REL=build/release
UNPACK=build/release-smoke
# 部分重建语义：只自检本次打包出的资产；缺失的资产由 release 上旧资产覆盖
# （完整性由 gen-meta.sh 的合并保证）。
# Partial-rebuild semantics: only smoke what was packed this run; absent assets
# remain covered by the release's existing ones (completeness via gen-meta merge).
HAVE_RUNTIME=false; [[ -f "$REL/rootfs-arm64.tar.gz" ]] && HAVE_RUNTIME=true

fail() { echo "SMOKE FAIL: $*" >&2; exit 1; }
pass() { echo "smoke ok: $*"; }
skip() { echo "smoke skip: $*（本次未重建 / not rebuilt this run）"; }

rm -rf "$UNPACK" && mkdir -p "$UNPACK"
if $HAVE_RUNTIME; then
echo ">> 解包 Runtime 树"
tar -C "$UNPACK" -xzf "$REL/rootfs-arm64.tar.gz"   # → rootfs-arm64.tar
mkdir -p "$UNPACK/tree"
tar -C "$UNPACK/tree" -xf "$UNPACK/rootfs-arm64.tar"
fi

echo ">> 1/3 Runtime 树存活检查（arm64 容器 + 设备路径）"
scripts/ensure-binfmt.sh >/dev/null
docker image inspect fexdroid-empty:arm64 >/dev/null 2>&1 || \
  tar -cf - --files-from /dev/null | docker import --platform linux/arm64 - fexdroid-empty:arm64 >/dev/null
TREE="$PWD/$UNPACK/tree"
# 基础程序可执行：bash 以设备路径运行
if $HAVE_RUNTIME; then
docker run --rm --platform linux/arm64 -v "$TREE:$FXD_ROOT" \
  --entrypoint "$FXD_ROOT/bin/bash" fexdroid-empty:arm64 -euc 'echo alive && test -x /bin/sh' \
  >/dev/null || fail "runtime tree cannot run bash at the device path"
# ldconfig 重建缓存（树内 our-ldconfig；来自 Debian 基底，安装到 usr/sbin）
docker run --rm --platform linux/arm64 -v "$TREE:$FXD_ROOT" \
  --entrypoint "$FXD_ROOT/usr/sbin/ldconfig" fexdroid-empty:arm64 -X \
  >/dev/null || fail "ldconfig failed on the runtime tree"
[[ -s "$UNPACK/tree/etc/ld.so.cache" ]] || fail "ld.so.cache was not produced"
rm -f "$UNPACK/tree/etc/ld.so.cache"   # 还原：发布的树不带本地生成的缓存
pass "runtime tree alive (bash + ldconfig at $FXD_ROOT)"
else
skip "runtime tree"
fi

echo ">> 2/3 组件关键产物"
GL="build/glibc/install"; FX="build/fex/install"; ME="build/mesa/install"
if [[ -d "$GL/data/data" ]]; then
  [[ -f "$GL$FXD_LDSO" ]] || fail "glibc install missing loader $FXD_LDSO"
  [[ -f "$GL$FXD_ROOT/usr/sbin/ldconfig" ]] || fail "glibc install missing ldconfig"
  pass "glibc loader + ldconfig present"
else
  skip "glibc install"
fi
if [[ -d "$FX/data/data" ]]; then
  [[ -f "$FX$FXD_ROOT/usr/bin/FEX" ]] || fail "fex install missing $FXD_ROOT/usr/bin/FEX"
  [[ -f "$FX$FXD_ROOT/usr/share/fex-emu/Config.json" ]] || fail "fex install missing default Config.json"
  pass "FEX interpreter + config present"
else
  skip "fex install"
fi
if [[ -d "$ME/data/data" ]]; then
  MESO="$ME$FXD_ROOT/usr/lib/$FXD_MULTIARCH/libvulkan_freedreno.so"
  ICD="$ME$FXD_ROOT/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json"
  [[ -f "$MESO" ]] || fail "mesa install missing Turnip ICD library"
  grep -q "\"library_path\": \"$FXD_ROOT/usr/lib/$FXD_MULTIARCH/libvulkan_freedreno.so\"" "$ICD" \
    || fail "mesa ICD library_path is not the absolute on-device path"
  pass "Turnip ICD present with device-path library_path"
else
  skip "mesa install"
fi

echo ">> 3/3 其余资产结构"
if [[ -d build/rootfs/sysroot-arm64/usr/include ]]; then pass "sysroot-arm64 include"; else skip "sysroot-arm64"; fi
if [[ -d build/rootfs/sysroot-x86_64/usr/include ]]; then pass "sysroot-x86_64 include"; else skip "sysroot-x86_64"; fi
if [[ -f "$REL/rootfs-x86_64.tar.gz" ]]; then
  mkdir -p "$UNPACK/x86" && tar -C "$UNPACK/x86" -xzf "$REL/rootfs-x86_64.tar.gz" \
    && tar -C "$UNPACK/x86" -xf "$UNPACK/x86/rootfs-x86_64.tar"
  [[ -d "$UNPACK/x86/usr/bin" ]] || fail "x86_64 guest tree missing usr/bin"
  pass "x86_64 guest tree"
else
  skip "x86_64 guest tree"
fi
if [[ -f "$REL/rootfs-arm64.tar.zst" ]]; then
  zstd -q -t "$REL/rootfs-arm64.tar.zst" || fail "device pack rootfs-arm64.tar.zst is corrupt"
  [[ -s "$REL/rootfs-arm64.uris.txt" ]] || fail "missing lock list rootfs-arm64.uris.txt"
  pass "device pack + lock list"
else
  skip "device pack"
fi

rm -rf "$UNPACK"
echo ">> SMOKE PASS"
