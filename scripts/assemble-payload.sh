#!/usr/bin/env bash
# Assembles the FINAL payload tars from the local component builds (extract-rootfs-repo
# 范围修订): components (Debian trees, patched glibc, FEX, Mesa) + rootfs-resident shims
# (fxshmd / libfxpath / libfexpad / fxwmfit / fxlsof / tests) -> the phone's Linux world.
# Outputs:
#   build/payload/assets/payload.version|payload.json   content hash + path manifest
#   build/payload-rootfs/rootfs-arm64.tar               the assembled runtime tree
#   build/payload-rootfs/rootfs-x86_64.tar              the assembled x86_64 guest tree
# APK-side jniLibs (libfxprobe / libunsquashfs) are NOT built here — they belong to the
# app repo (build-jnilibs.sh), which downloads THIS script's products.
# app 仓库（fetch-payload.sh + build-jnilibs.sh）消费本脚本产物；本脚本不产 jniLibs。
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh
if [[ -n "${FEXPAD_ONLY_OUT:-}" ]]; then
  FEXPAD_SYSROOT="${FEXPAD_SYSROOT:-build/rootfs/sysroot-arm64}"
  mkdir -p "$(dirname "$FEXPAD_ONLY_OUT")"
  "${FEXPAD_CC:-clang}" --target=aarch64-linux-gnu --sysroot="$FEXPAD_SYSROOT" -fuse-ld=lld -O2 -Wall \
    -shared -fPIC -o "$FEXPAD_ONLY_OUT" tools/fexpad/libfexpad.c -ldl -lpthread
  exit
fi
OUT=build/payload
STAGE=build/payload-stage
rm -rf "$OUT" "$STAGE"
KEEP_STAGE="${KEEP_STAGE:-1}"   # stage is reused by scripts/test-payload-qemu.sh
mkdir -p "$OUT/assets" "$STAGE/arm64" "$STAGE/x86_64"

# 缺失前置产物时指名文件与产出方：下游的 "cd: build/payload-rootfs: No such file
# or directory" 正是这条诊断缺失时冒出来的间接错误。
# Name the missing file and its producer: the downstream
# "cd: build/payload-rootfs: No such file or directory" is exactly the indirect
# symptom that shows up when this check is silent.
need() { [[ -e "$1" ]] || { echo "missing prerequisite: $1" >&2; echo "  produced by: $2" >&2; exit 1; }; }
need build/rootfs/rootfs-arm64.tar "scripts/build-rootfs-image.sh"
need build/rootfs/rootfs-x86_64.tar "scripts/build-rootfs.sh"
need "build/glibc/install$FXD_LDSO" "scripts/build-glibc.sh"
need "build/fex/install$FXD_ROOT/usr/bin/FEX" "scripts/build-fex.sh"

# jniLibs（libfxprobe / libunsquashfs）归 app 仓库 build-jnilibs.sh：它们随 APK 分发、
# 从 nativeLibraryDir 加载，不进 rootfs。
# jniLibs (libfxprobe / libunsquashfs) belong to the app repo's build-jnilibs.sh: they
# ship with the APK and load from nativeLibraryDir — never into the rootfs.

# ---- arm64 rootfs ------------------------------------------------------------------
A="$STAGE/arm64"
tar -C "$A" -xf build/rootfs/rootfs-arm64.tar
# Patched glibc runtime (same ABI as Debian's libc6, see scripts/build-glibc.sh).
G="build/glibc/install$FXD_LIBDIR"
for f in "$G"/*.so* ; do
  [[ -f "$f" ]] || continue
  case "$(basename "$f")" in *.a|libc.so|libm.so|libpthread.so|libmvec.so) continue;; esac
  cp -a "$f" "$A/usr/lib/$FXD_MULTIARCH/"
done
[[ -d "$G/gconv" ]] && cp -a "$G/gconv/." "$A/usr/lib/$FXD_MULTIARCH/gconv/"
# FEX.
cp -a "build/fex/install$FXD_ROOT/." "$A/"
# Mesa Turnip (KGSL) + ICD manifest with the absolute on-device library path.
need "build/mesa/install$FXD_ROOT/usr/lib/$FXD_MULTIARCH/libvulkan_freedreno.so" "scripts/build-mesa.sh"
cp -a "build/mesa/install$FXD_ROOT/." "$A/"
# Drop debug info from what we compiled ourselves (glibc, FEX); keeps dynamic symbols.
# llvm-strip comes from PATH (CI: llvm-22; local: brew llvm).
find "$A/usr/lib/$FXD_MULTIARCH" -maxdepth 1 -newer build/rootfs/rootfs-arm64.tar -type f -name '*.so*' \
  -exec llvm-strip --strip-debug {} +
find "$A/usr/bin" -maxdepth 1 -type f -name 'FEX*' -exec llvm-strip --strip-debug {} +
find "$A/usr/lib" -maxdepth 1 -type f -name 'libFEXCore.so' -exec llvm-strip --strip-debug {} +
# fxshmd: SysV shared memory + semaphore daemon started on demand by the patched
# glibc (patches/glibc/0002-*, NOTES.md N-017, N-025), plus its test programs.
tools/fxshmd/build.sh >/dev/null
install -D -m 0755 build/fxshmd/fxshmd "$A/usr/libexec/fxshmd"
install -D -m 0755 build/fxshmd/shmtest "$A/opt/fexdroid-tests/shmtest"
install -D -m 0755 build/fxshmd/semtest "$A/opt/fexdroid-tests/semtest"
# Directories glibc was built to use instead of /tmp, /dev/shm, /etc.
mkdir -p "$A/tmp" "$A/var/tmp" "$A/dev/shm" "$A/home"
# Empty sysfs PCI tree FEX shows guests as /sys/bus/pci (patches/fex/0002, NOTES.md N-028).
mkdir -p "$A/usr/share/fex-emu/fexdroid-empty-pci/devices" "$A/usr/share/fex-emu/fexdroid-empty-pci/drivers"
chmod 1777 "$A/tmp" "$A/var/tmp" "$A/dev/shm"

# Point every dynamically linked ELF at the on-device loader, and rewrite
# absolute RUNPATHs (/usr/lib/...) into the rootfs.
python3 scripts/lib/fix-elf.py "$A" "$FXD_ROOT" "$FXD_LDSO"
# Debian's Vulkan manifests (lavapipe) name /usr/lib/...: point them into the rootfs.
python3 - "$A/usr/share/vulkan/icd.d" "$FXD_ROOT" <<'PY'
import json, pathlib, sys
d, root = pathlib.Path(sys.argv[1]), sys.argv[2]
for f in d.glob("*.json"):
    j = json.loads(f.read_text())
    p = j.get("ICD", {}).get("library_path", "")
    if p.startswith("/") and not p.startswith(root):
        j["ICD"]["library_path"] = root + p
        f.write_text(json.dumps(j, indent=4) + "\n")
        print(f"icd {f.name}: {j['ICD']['library_path']}")
PY

# ld.so.cache: Debian's cache and ld.so.conf name /lib/..., which does not exist on
# Android. Rewrite the config with rootfs paths and regenerate the cache with our
# ldconfig (it reads $FXD_ROOT/etc/ld.so.conf), running it under qemu at the device path.
cp "build/glibc/install$FXD_ROOT/usr/sbin/ldconfig" "$A/usr/sbin/ldconfig"
echo "include $FXD_ROOT/etc/ld.so.conf.d/*.conf" > "$A/etc/ld.so.conf"
sed -i -E "s|^/|$FXD_ROOT/|" "$A"/etc/ld.so.conf.d/*.conf
rm -f "$A/etc/ld.so.cache"
scripts/ensure-binfmt.sh >/dev/null
docker image inspect fexdroid-empty:arm64 >/dev/null 2>&1 || \
  tar -cf - --files-from /dev/null | docker import --platform linux/arm64 - fexdroid-empty:arm64 >/dev/null
docker run --rm --platform linux/arm64 -v "$PWD/$A:$FXD_ROOT" --entrypoint "$FXD_ROOT/usr/sbin/ldconfig" \
  fexdroid-empty:arm64 -X
[[ -s "$A/etc/ld.so.cache" ]] || { echo "ldconfig did not produce a cache"; exit 1; }

# ---- thunks: host-side dlopen self-test (arm64) ---------------------------------------
# BEGIN thunks section. Host/guest thunk libraries, ThunksDB.json and the default
# Config.json come with build/fex/install (copied above). Only a self-test binary is
# added here; run it with:
#   scripts/test-payload-qemu.sh "$FXD_ROOT/opt/fexdroid-tests/dlopen-thunks $FXD_ROOT/usr/lib/fex-emu/HostThunks"
mkdir -p "$A/opt/fexdroid-tests"
clang --target=aarch64-linux-gnu --sysroot="$PWD/build/rootfs/sysroot-arm64" -fuse-ld=lld \
  -Wl,--dynamic-linker="$FXD_LDSO" -O2 -Wall -o "$A/opt/fexdroid-tests/dlopen-thunks" tests/arm64/dlopen-thunks.c -ldl
# END thunks section.

# fxpath: LD_PRELOAD shim mapping /tmp, /usr, /etc, ... into the rootfs for programs
# with hardcoded paths (Xvfb, xkbcomp, PulseAudio). See tools/fxpath/fxpath.c.
mkdir -p "$A/usr/lib/fexdroid"
clang --target=aarch64-linux-gnu --sysroot=build/rootfs/sysroot-arm64 -fuse-ld=lld -O2 -Wall \
  -shared -fPIC -o "$A/usr/lib/fexdroid/libfxpath.so" tools/fxpath/fxpath.c -ldl

# fexpad: LD_PRELOAD shim that gives native (arm64) guests a virtual gamepad, so a
# ported game finds a controller without any code of its own. See tools/fexpad/libfexpad.c.
clang --target=aarch64-linux-gnu --sysroot=build/rootfs/sysroot-arm64 -fuse-ld=lld -O2 -Wall \
  -shared -fPIC -o "$A/usr/lib/fexdroid/libfexpad.so" tools/fexpad/libfexpad.c -ldl -lpthread

# fxwmfit: keeps top-level windows inside the virtual screen (no window manager on the phone).
clang --target=aarch64-linux-gnu --sysroot=build/rootfs/sysroot-arm64 -fuse-ld=lld \
  -Wl,--dynamic-linker="$FXD_LDSO" -O2 -Wall -o "$A/usr/bin/fxwmfit" tools/fxwmfit/fxwmfit.c -lX11

# Test for the glibc seccomp workarounds (patches/glibc/0003).
clang --target=aarch64-linux-gnu --sysroot=build/rootfs/sysroot-arm64 -fuse-ld=lld -O2 \
  -Wl,--dynamic-linker="$FXD_LDSO" -o "$A/opt/fexdroid-tests/seccomp-wrap" tests/arm64/seccomp-wrap.c
# Phase 4 audio test tone (arm64; the x86_64 build goes into the guest rootfs below).
clang --target=aarch64-linux-gnu --sysroot=build/rootfs/sysroot-arm64 -fuse-ld=lld -O2 \
  -Wl,--dynamic-linker="$FXD_LDSO" -o "$A/opt/fexdroid-tests/tone" tests/common/tone.c -lm
# Reads Xvfb's -shmem framebuffer header through fxshmd, like the app's display bridge.
clang --target=aarch64-linux-gnu --sysroot=build/rootfs/sysroot-arm64 -fuse-ld=lld -O2 \
  -Wl,--dynamic-linker="$FXD_LDSO" -o "$A/opt/fexdroid-tests/xwd-peek" tests/arm64/xwd-peek.c

# ---- x86_64 guest rootfs --------------------------------------------------------
X="$STAGE/x86_64"
tar -C "$X" -xf build/rootfs/rootfs-x86_64.tar
# Absolute symlinks would escape the RootFS on the phone (NOTES.md N-018).
python3 scripts/lib/relativize-symlinks.py "$X"
mkdir -p "$X/opt/fexdroid-tests"
gcc -O2 -static -o "$X/opt/fexdroid-tests/hello-static" tests/x86/hello.c
gcc -O2 -o "$X/opt/fexdroid-tests/hello-dynamic" tests/x86/hello.c
gcc -O2 -o "$X/opt/fexdroid-tests/tone" tests/common/tone.c -lm
# SysV semaphores from an x86-64 guest: FEX semget/semop/semtimedop -> glibc -> fxshmd (N-025).
# lsof for the guests' websocket peer checks, answered from FEX's TCP records (tools/lsof/fxlsof.c).
# The app copies it into the x86 guest trees (LinuxEnv.writeIdentityFiles).
gcc -O2 -Wall -o "$X/usr/bin/lsof" tools/lsof/fxlsof.c
# The same for native arm64 programs, whose sockets libfxpath records.
clang --target=aarch64-linux-gnu --sysroot=build/rootfs/sysroot-arm64 -fuse-ld=lld \
  -Wl,--dynamic-linker="$FXD_LDSO" -O2 -Wall -o "$A/usr/bin/lsof" tools/lsof/fxlsof.c
# Dynamic: static x86 binaries crash under FEX on older kernels (NOTES N-021).
gcc -O2 -o "$X/opt/fexdroid-tests/semtest" tests/sysvsem/semtest.c
# i386 variant (direct semget/semctl/semtimedop_time64 syscalls, FEX's x32 handlers); needs a
# 32-bit static libc on the build host, optional.
gcc -m32 -O2 -o "$X/opt/fexdroid-tests/semtest-i386" tests/sysvsem/semtest.c 2>/dev/null || \
  echo "note: no 32-bit static libc on this host, semtest-i386 not built"

# ---- pack --------------------------------------------------------------------------
# 内部名保留 rootfs-*.tar（app 解包/ push-rootfs.sh 的既有约定）；发布资产名
# payload-*.tar.gz 由 pack-release-assets.sh 决定（单条目装包）。
# Internal names stay rootfs-*.tar (the app extract / push-rootfs.sh convention);
# the RELEASE asset names payload-*.tar.gz are decided by pack-release-assets.sh.--
# The rootfs tars go into build/payload-rootfs/ (picked up by release builds only, see
# app/build.gradle.kts): debug APKs stay small, the rootfs is pushed by hand instead.
ROOTFS_OUT=build/payload-rootfs
rm -rf "$ROOTFS_OUT"
mkdir -p "$ROOTFS_OUT"
for n in arm64 x86_64; do
  tar -C "$STAGE/$n" --numeric-owner --owner=0 --group=0 --sort=name --mtime='@0' \
      -cf "$ROOTFS_OUT/rootfs-$n.tar" .
done
( cd "$ROOTFS_OUT" && sha256sum rootfs-*.tar | sha256sum | cut -c1-16 ) > "$OUT/assets/payload.version"
echo "{\"root\":\"$FXD_ROOT\",\"ldso\":\"$FXD_LDSO\"}" > "$OUT/assets/payload.json"
ls -la "$OUT/assets" "$ROOTFS_OUT"
