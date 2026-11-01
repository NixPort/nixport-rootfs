#!/usr/bin/env bash
# Cross-builds FEX-Emu for the arm64 glibc rootfs with host clang + the Debian sysroot.
# Binaries get the on-device ld.so as ELF interpreter, so they run directly on the
# phone without patchelf.
#
# Thunks (BUILD_THUNKS=ON) are built too. Three toolchains are involved:
#   thunkgen        native x86_64 build-machine tool (links the host's libclang);
#                   built separately from scripts/cmake/thunkgen/ and handed to FEX
#                   via THUNKGEN_EXE (local patch patches/fex/0001-*.patch)
#   host thunks     arm64, same cross toolchain as FEX, installed to
#                   $FXD_ROOT/usr/lib/fex-emu/HostThunks/lib<name>-host.so
#   guest thunks    x86_64, clang + scripts/cmake/toolchain-x86_64-guest.cmake against
#                   build/rootfs/sysroot-x86_64 (scripts/build-x86-sysroot.sh), installed
#                   to $FXD_ROOT/usr/share/fex-emu/GuestThunks/lib<name>-guest.so
# See NOTES.md N-0xx "Thunks" for the runtime layout and configuration.
#
# Output: build/fex/install/  (DESTDIR tree under $FXD_ROOT/usr)
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh
FEX_TAG="${FEX_TAG:-FEX-2609}"
SRC=build/fex/src
BLD=build/fex/obj
export SYSROOT="$PWD/build/rootfs/sysroot-arm64"
export X86_SYSROOT="$PWD/build/rootfs/sysroot-x86_64"
# 缺失前置产物时指名文件与产出方，别让下游报出无关的间接错误。
# Name the missing file and its producer, so downstream steps don't fail with
# an unrelated symptom.
# shellcheck source=scripts/lib/need-artifact.sh
source scripts/lib/need-artifact.sh
need_artifact "$SYSROOT/usr/include" scripts/build-rootfs.sh
need_artifact "$X86_SYSROOT/usr/include" scripts/build-x86-sysroot.sh

if [[ ! -d "$SRC/.git" ]]; then
  git clone -q --depth 1 --branch "$FEX_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/FEX-Emu/FEX.git "$SRC"
fi
git -C "$SRC" describe --tags --exact-match | grep -qx "$FEX_TAG" || { echo "source is not at $FEX_TAG"; exit 1; }

# Apply local patches (never upstreamed: FEX does not accept AI-written code).
# The tree is reset to the tag first so an edited patch never silently stays stale.
# `clean` also drops files a previous patch added, otherwise the re-apply fails.
git -C "$SRC" checkout -q -- . && git -C "$SRC" clean -fdq
git -C "$SRC" submodule -q foreach --recursive 'git checkout -q -- . && git clean -fdq'
for p in patches/fex/*.patch; do
  [[ -e "$p" ]] || continue
  git -C "$SRC" apply "$PWD/$p" || { echo "patch does not apply: $p"; exit 1; }
  echo "applied $p"
done

# ---- extra arm64 Debian packages for the thunk host side --------------------------
# ALSA is not in scripts/build-rootfs.sh's package lists. The -dev package goes into
# the cross sysroot; the runtime package is extracted into the install tree so
# build-payload.sh ships it next to libasound-host.so. Same archive as the rootfs.
DEBS=build/fex/debs-arm64
THUNK_HOST_DEV_DEBS=(libasound2-dev)
THUNK_HOST_RUNTIME_DEBS=(libasound2t64 libasound2-data)
if [[ ! -f "$DEBS/.done" ]]; then
  APT_SNAPSHOT=""; [[ -n "${SNAPSHOT:-}" ]] && APT_SNAPSHOT="--snapshot $SNAPSHOT"
  rm -rf "$DEBS" && mkdir -p "$DEBS"
  scripts/ensure-binfmt.sh >/dev/null
  docker run --rm --platform linux/arm64 -v "$PWD/$DEBS:/out" debian:trixie-slim bash -euc "
    apt-get update $APT_SNAPSHOT -qq >/dev/null
    cd /out && apt-get download ${THUNK_HOST_DEV_DEBS[*]} ${THUNK_HOST_RUNTIME_DEBS[*]} >/dev/null 2>&1
    chown -R $(id -u):$(id -g) /out"
  ls "$DEBS"/*.deb > "$DEBS/.done"
fi
deb_extract() { # $1 = .deb, $2 = destination dir (no dpkg on the build host)
  ar p "$1" "$(ar t "$1" | grep '^data\.tar')" | bsdtar -xf - -C "$2"
}
for pkg in "${THUNK_HOST_DEV_DEBS[@]}"; do
  deb_extract "$DEBS"/${pkg}_*.deb "$SYSROOT"   # idempotent overlay onto the sysroot
done

# ---- thunkgen (native) -------------------------------------------------------------
# thunkgen 链接宿主机的 libclang，FEX 的 ThunkLibs/Generator 里是
# find_package(Clang REQUIRED CONFIG) + find_package(LLVM REQUIRED CONFIG)。
# 不指定路径时 CMake 按默认顺序搜索，会先命中宿主系统自带的 LLVM —— Ubuntu 24.04
# runner 镜像预装 clang-17 与 libclang-17-dev，带来了 /usr/lib/cmake/clang-17/
# ClangConfig.cmake，抢在 workflow 装的 llvm-22 前面。而那份 libclang-17-dev 不含
# libclangBasic.a，于是报：
#   "The imported target clangBasic references /usr/lib/llvm-17/lib/libclangBasic.a
#    but this file does not exist"
# 这与本仓库的改动无关：workflow 的 LLVM 安装段一直没变，变的是 runner 镜像
# （预装的 llvm-17 出现了）。修法是显式告诉 CMake 用哪一套 LLVM。
#
# thunkgen links the build machine's libclang; FEX's ThunkLibs/Generator does
# find_package(Clang REQUIRED CONFIG) + find_package(LLVM REQUIRED CONFIG).
# Without an explicit path, CMake's default search order picks the host's own LLVM:
# the Ubuntu 24.04 runner image ships clang-17 and libclang-17-dev, so
# /usr/lib/cmake/clang-17/ClangConfig.cmake wins over the workflow's llvm-22. That
# libclang-17-dev ships no libclangBasic.a, hence:
#   "The imported target clangBasic references /usr/lib/llvm-17/lib/libclangBasic.a
#    but this file does not exist"
# Not caused by anything in this repo: the workflow's LLVM setup never changed, the
# runner image did (it gained a preinstalled llvm-17). Point CMake at the LLVM we want.
#
# 探测方式：不走 llvm-config。它是 shell 脚本，靠自身路径推导 prefix，而 workflow 把
# /usr/lib/llvm-22/bin/* 软链进了 /usr/local/bin，软链后它算出的 prefix 是错的
# （实测 --cmakedir 会指向 /usr/local/lib/cmake/llvm）。改为直接在 /usr/lib/llvm-*/
# 里挑出带 ClangConfig.cmake 且带 libclangBasic.a 的那套。
# Probing: not via llvm-config. It is a shell script that derives its prefix from its
# own path, and the workflow symlinks /usr/lib/llvm-22/bin/* into /usr/local/bin, so
# after symlinking it computes the WRONG prefix (measured: --cmakedir points at
# /usr/local/lib/cmake/llvm). Pick the LLVM under /usr/lib/llvm-*/ directly instead:
# the one that has both ClangConfig.cmake and libclangBasic.a.
LLVM_CMAKE_DIR=""
LLVM_PREFIX=""
for d in /usr/lib/llvm-*/; do
  [[ -f "$d/lib/cmake/clang/ClangConfig.cmake" && -f "$d/lib/libclangBasic.a" ]] || continue
  LLVM_CMAKE_DIR="${d}lib/cmake/llvm"
  LLVM_PREFIX="${d%/}"
  break
done
if [[ -z "$LLVM_CMAKE_DIR" ]]; then
  echo "no usable LLVM under /usr/lib/llvm-*: need ClangConfig.cmake and libclangBasic.a" >&2
  echo "  install the matching dev packages (workflow: libclang-22-dev llvm-22-dev)" >&2
  exit 1
fi
CLANG_CMAKE_DIR="$(dirname "$LLVM_CMAKE_DIR")/clang"
# ClangConfig/llvm-config 之外的第三个版本耦合点：thunkgen 的 CMakeLists 会
# find_program(CLANG_EXEC_PATH clang) 再问它 -print-resource-dir。PATH 上未必是
# 这套 LLVM 的 clang（系统自带 llvm-17 的 clang 也在 /usr/bin），拿到不同版本的
# resource dir 会和库对不上。这里直接问选中这套自带的 clang，并把结果显式传进去。
# A third version coupling: thunkgen's CMakeLists does find_program(CLANG_EXEC_PATH
# clang) and asks it for -print-resource-dir. The clang on PATH may belong to a
# different LLVM (Ubuntu's llvm-17 clang lives in /usr/bin), and a mismatched
# resource dir does not agree with the libraries. Ask the clang from the selected
# LLVM and pass the result in explicitly.
LLVM_CLANG="$LLVM_PREFIX/bin/clang"
CLANG_RESOURCE_DIR=""
if [[ -x "$LLVM_CLANG" ]]; then
  CLANG_RESOURCE_DIR="$("$LLVM_CLANG" -print-resource-dir 2>/dev/null || true)"
fi
echo ">> thunkgen: LLVM at $LLVM_CMAKE_DIR (clang resource dir: ${CLANG_RESOURCE_DIR:-unset})"
cmake -S scripts/cmake/thunkgen -B build/fex/thunkgen -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFEX_SOURCE_DIR="$PWD/$SRC" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DClang_DIR="$CLANG_CMAKE_DIR" -DLLVM_DIR="$LLVM_CMAKE_DIR" \
  ${CLANG_RESOURCE_DIR:+-DCLANG_RESOURCE_DIR="$CLANG_RESOURCE_DIR"} \
  >build/fex/thunkgen-cmake.log
ninja -C build/fex/thunkgen thunkgen >build/fex/thunkgen-ninja.log 2>&1 || { tail -40 build/fex/thunkgen-ninja.log; exit 1; }
THUNKGEN="$PWD/build/fex/thunkgen/thunkgen/thunkgen"

# ---- FEX + host thunks (arm64) + guest thunks (x86_64) ---------------------------------
# pkg-config for the host-side configure answers from the arm64 sysroot; the guest
# toolchain file overrides these for the guest-libs sub-build.
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/$FXD_MULTIARCH/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
unset PKG_CONFIG_PATH
# thunkgen runs on x86_64 but must parse the *host* interfaces with the aarch64 ABI
# (char is unsigned there) and the arm64 sysroot headers; its guest-layout pass appends
# its own --target=x86_64-linux-gnu afterwards, which takes precedence.
export THUNKGEN_EXTRA_FLAGS="--target=aarch64-linux-gnu --sysroot=$SYSROOT"
cmake -S "$SRC" -B "$BLD" -G Ninja \
  --toolchain "$PWD/$SRC/Data/CMake/toolchain_aarch64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$FXD_ROOT/usr" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -Wl,--dynamic-linker=$FXD_LDSO" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=lld" \
  -DUSE_LINKER=lld \
  -DTUNE_CPU=generic -DTUNE_ARCH=armv8.2-a \
  -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF \
  -DBUILD_THUNKS=ON -DENABLE_CLANG_THUNKS=ON -DTHUNKS_ENABLE_GL=OFF -DTHUNKS_ENABLE_32BIT_GUEST=OFF \
  -DTHUNKGEN_EXE="$THUNKGEN" \
  -DX86_DEV_ROOTFS="$X86_SYSROOT" \
  -DX86_64_TOOLCHAIN_FILE="$PWD/scripts/cmake/toolchain-x86_64-guest.cmake" \
  -DENABLE_LTO=ON -DENABLE_CCACHE=ON \
  -DOVERRIDE_VERSION="${FEX_TAG#FEX-}" >build/fex/cmake.log
# THUNKS_ENABLE_32BIT_GUEST=OFF: the guest rootfs has no i386 multiarch yet (phase 5).
ninja -C "$BLD" >build/fex/ninja.log 2>&1 || { tail -40 build/fex/ninja.log; exit 1; }
rm -rf build/fex/install
DESTDIR="$PWD/build/fex/install" ninja -C "$BLD" install >/dev/null
INST="build/fex/install$FXD_ROOT"

# Host-side runtime libraries the host thunks dlopen (ALSA; X11/xcb/vulkan are already
# in the rootfs from build-rootfs.sh). Shipped via build-payload.sh like the rest.
for pkg in "${THUNK_HOST_RUNTIME_DEBS[@]}"; do
  deb_extract "$DEBS"/${pkg}_*.deb "$INST"
done
rm -rf "$INST/usr/share/doc" "$INST/usr/share/lintian"

# Default (global) FEX config: enables the Vulkan thunk for every guest. Read from
# $FXD_ROOT/usr/share/fex-emu/Config.json (= DATA_DIRECTORY, the "global" config
# layer); the per-user file $HOME/.fex-emu/Config.json (FEX_APP_CONFIG_LOCATION)
# overrides it. "ThunksDB" names refer to entries in ThunksDB.json next to it.
cat > "$INST/usr/share/fex-emu/Config.json" <<JSON
{
  "Config": {
    "RootFS": "$(dirname "$FXD_ROOT")/x86_64",
    "ThunkHostLibs": "$FXD_ROOT/usr/lib/fex-emu/HostThunks",
    "ThunkGuestLibs": "$FXD_ROOT/usr/share/fex-emu/GuestThunks"
  },
  "ThunksDB": {
    "Vulkan": 1,
    "drm": 0,
    "asound": 0,
    "WaylandClient": 0
  }
}
JSON
find build/fex/install -type f -name 'FEX*' -perm -u+x | head
find build/fex/install -name '*-host.so' -o -name '*-guest.so' | sort
