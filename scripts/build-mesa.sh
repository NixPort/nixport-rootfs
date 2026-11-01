#!/usr/bin/env bash
# Cross-builds Mesa Turnip (Vulkan for Adreno) with the KGSL kernel backend for the
# arm64 glibc rootfs. Android has no DRM/msm node for apps, only /dev/kgsl-3d0.
#
# Target GPU: Adreno 840 (a8xx gen2, Snapdragon 8 Elite Gen 5). Mesa 26.2.3 already
# carries the upstream A840 device entry (chip_id 0xffff44050A31, upstream commit
# 6e359817, Rob Clark/Qualcomm); patches/mesa/*.patch add a KGSL patch-id wildcard
# fallback. See NOTES.md N-015 for the analysis and sources.
#
# Reproducibility: release tarball pinned by sha256, source tree re-extracted and
# re-patched on every run, meson wrap fallbacks disabled (no network at configure).
#
# Output: build/mesa/install/  (DESTDIR tree under $FXD_ROOT/usr)
#   libvulkan_freedreno.so + freedreno_icd.aarch64.json (library_path absolute on device)
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh
MESA_VERSION="${MESA_VERSION:-26.2.3}"
# sha256 of the release tarball, recorded on first download (archive.mesa3d.org has no .sha256).
MESA_SHA256="${MESA_SHA256:-1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f}"
SYSROOT="$PWD/build/rootfs/sysroot-arm64"
SRC="build/mesa/mesa-$MESA_VERSION"
BLD="build/mesa/obj"
TARBALL="build/mesa/mesa-$MESA_VERSION.tar.xz"
# 缺失前置产物时指名文件与产出方，别让下游报出无关的间接错误。
# Name the missing file and its producer, so downstream steps don't fail with
# an unrelated symptom.
# shellcheck source=scripts/lib/need-artifact.sh
source scripts/lib/need-artifact.sh
need_artifact "$SYSROOT/usr/include" scripts/build-rootfs.sh
mkdir -p build/mesa
if [[ ! -f "$TARBALL" ]]; then
  curl -sSL "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz" -o "$TARBALL"
fi
echo "$MESA_SHA256  $TARBALL" | sha256sum -c -

# Always start from a pristine tree so the patch set is exactly patches/mesa/*.patch.
rm -rf "$SRC"
tar -C build/mesa -xf "$TARBALL"
# Local patches (documented in NOTES.md, licenses in LICENSES.md), applied in order.
for p in patches/mesa/*.patch; do
  [[ -e "$p" ]] || continue
  patch -d "$SRC" -p1 --no-backup-if-mismatch -s < "$p" && echo "applied $p"
done
# Nothing in libvulkan_freedreno.so needs the wrap subprojects (libarchive/libxml2 are
# only for the freedreno decode tools); make sure stale downloads cannot sneak back in.
rm -rf "$SRC"/subprojects/libarchive-* "$SRC"/subprojects/libxml2-* "$SRC"/subprojects/packagecache

# cpu must be 'aarch64': meson names the manifest freedreno_icd.<cpu>.json and
# vk_icd_gen.py only writes an absolute library_path when that name has exactly
# three dot-separated parts ('armv8.2-a' would break it and yield a relative path).
cat > build/mesa/cross-aarch64.ini <<EOC
[binaries]
c = ['clang', '--target=aarch64-linux-gnu', '--sysroot=$SYSROOT']
cpp = ['clang++', '--target=aarch64-linux-gnu', '--sysroot=$SYSROOT']
c_ld = 'lld'
cpp_ld = 'lld'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
python = '/usr/bin/python3'

[built-in options]
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld']

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = ['$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig', '$SYSROOT/usr/share/pkgconfig']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOC

# Build-machine tools meson must find natively (mako via the system Python).
export PATH="/usr/bin:$PATH"
rm -rf "$BLD"
meson setup "$BLD" "$SRC" --cross-file build/mesa/cross-aarch64.ini \
  --prefix="$FXD_ROOT/usr" --libdir="lib/$FXD_MULTIARCH" \
  --wrap-mode=nofallback \
  -Dbuildtype=release -Db_ndebug=true \
  -Dplatforms=x11 \
  -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
  -Dgallium-drivers= -Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled \
  -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dzstd=enabled -Dvulkan-layers= -Dtools= -Dbuild-tests=false \
  >build/mesa/meson.log 2>&1 || { tail -40 build/mesa/meson.log; exit 1; }
if grep -q "Executing subproject" build/mesa/meson.log; then
  echo "error: meson pulled in a wrap subproject:"; grep "Executing subproject" build/mesa/meson.log; exit 1
fi
ninja -C "$BLD" >build/mesa/ninja.log 2>&1 || { tail -40 build/mesa/ninja.log; exit 1; }
rm -rf build/mesa/install
DESTDIR="$PWD/build/mesa/install" meson install -C "$BLD" --no-rebuild >/dev/null

# --- Verification (host side; nothing here proves it runs on the phone) -------------
SO="build/mesa/install$FXD_ROOT/usr/lib/$FXD_MULTIARCH/libvulkan_freedreno.so"
ICD="build/mesa/install$FXD_ROOT/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json"
GEN="$BLD/src/freedreno/common/freedreno_devices.h"
[[ -f "$SO" && -f "$ICD" ]] || { echo "error: missing $SO or $ICD"; find build/mesa/install -type f; exit 1; }
grep -q "\"library_path\": \"$FXD_ROOT/usr/lib/$FXD_MULTIARCH/libvulkan_freedreno.so\"" "$ICD" \
  || { echo "error: ICD library_path is not the absolute on-device path:"; cat "$ICD"; exit 1; }
# A840 must be in the generated device table and its name string in the binary.
grep -qi "0xffff44050a31" "$GEN" || { echo "error: A840 chip id missing from $GEN"; exit 1; }
grep -q "Adreno (TM) 840" "$SO" || { echo "error: A840 name string missing from $SO"; exit 1; }
echo "A840 entries in generated device table:"; grep -in "44050a" "$GEN" | sed 's|^| |'
file "$SO" | sed 's|^| |'
find build/mesa/install -type f | sed 's|^| |'
