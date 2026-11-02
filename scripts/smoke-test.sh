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

REL=build/release  # 渠道目录：payload/ sysroot/ rootfs/
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

echo ">> 1/3 Runtime 树存活检查（chroot：树自身就是根）"
# 方法学（首轮 CI 教训）：裸 Debian 树的 ELF 解释器还是标准路径（/lib/ld-...），
# 路径改写是 app 侧 build-payload.sh 的活——用空镜像按设备路径挂载会因解释器
# 不在挂载点内而假失败。chroot 让树自己当根，/lib 在树内，检查的是树的真实
# 存活能力。chroot 在 docker 默认 capability 集内，无需 --privileged。
# Method (first CI round lesson): the raw Debian tree's ELFs still use standard
# interpreters (/lib/ld-...) — rewriting to device paths is app-side
# build-payload.sh's job. Mounting the tree at the device path in an EMPTY image
# fails because the interpreter is outside the mount. chroot makes the tree its
# own root so /lib resolves inside it; chroot is within docker's default caps.
scripts/ensure-binfmt.sh >/dev/null
TREE="$PWD/$UNPACK/tree"
if $HAVE_RUNTIME; then
docker run --rm --platform linux/arm64 -v "$TREE:/smoke-root" \
  --entrypoint /bin/bash debian:trixie-slim -euc '
    chroot /smoke-root /bin/bash -euc "echo alive && test -x /bin/sh"
    chroot /smoke-root /usr/sbin/ldconfig -X
    test -s /smoke-root/etc/ld.so.cache
  ' >/dev/null || fail "runtime tree cannot run bash / ldconfig in chroot"
pass "runtime tree alive (bash + ldconfig in chroot)"
# 哨兵包断言（uris 锁定安装 bug 的兜底）：清单是生命线，抽 5 个必在的代表包 +
# 总包数下限。基底只有 ~78 包，完整 Runtime ≥ 300。
# Sentinel packages (the uris-lock bug's safety net): spot-check 5 representative
# packages + a total floor. Base alone is ~78; a full Runtime is ≥ 300.
NP="$(grep -c '^Package:' "$UNPACK/tree/var/lib/dpkg/status")"
[[ "$NP" -ge 250 ]] || fail "runtime tree has only $NP packages (manifest install broke again?)"
for s in python3.13 xvfb fonts-noto-cjk libgbm1 squashfs-tools; do
  grep -q "^Package: $s\$" "$UNPACK/tree/var/lib/dpkg/status" || fail "sentinel package missing: $s"
done
# AppImage 的静态 runtime（导入期解包的第一级途径）必须在树里：它不进包表，
# 靠 fetch-uruntime.sh 装进来，漏了不会有 dpkg 记录可查。
# The static AppImage runtime (extraction route ①) must be in the tree: it does
# not ride the package list, so a missing one leaves no dpkg trace.
[[ -x "$UNPACK/tree/usr/local/bin/uruntime" ]] || fail "rootfs tree lacks /usr/local/bin/uruntime"
pass "package manifest sane ($NP packages, sentinels present)"
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

echo ">> 2.5/3 成品 payload 自测（qemu：seccomp-wrap / shmtest，spec 发布前自检）"
# assemble-payload.sh 以 KEEP_STAGE=1 留下的暂存树就是发布内容的原样 —— 在这里
# 用 qemu 把运行时自测跑掉，app release 链因此不再需要 docker/qemu。
# The staging tree assemble-payload.sh leaves (KEEP_STAGE=1) IS the publish
# content — run the runtime self-tests here under qemu so the app release chain
# never needs docker/qemu.
if [[ -d build/payload-stage/arm64 ]]; then
  scripts/test-payload-qemu.sh "/data/data/$FXD_PACKAGE/files/rootfs/opt/fexdroid-tests/seccomp-wrap" 2>&1 | tail -5 \
    || fail "payload self-test failed: seccomp-wrap"
  scripts/test-payload-qemu.sh "/data/data/$FXD_PACKAGE/files/rootfs/opt/fexdroid-tests/shmtest all" 2>&1 | tail -5 \
    || fail "payload self-test failed: shmtest"
  pass "payload qemu self-tests (seccomp-wrap + shmtest)"
else
  skip "payload self-tests（本次未组装 / assembly not run this time）"
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

rm -rf "$UNPACK"
echo ">> SMOKE PASS"
