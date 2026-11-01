# CMake toolchain for the x86_64 *guest* side of the FEX thunks (ThunkLibs/GuestLibs).
# Passed to FEX as X86_64_TOOLCHAIN_FILE by scripts/build-fex.sh; FEX's ExternalProject
# forwards it to the guest-libs sub-build. Uses the host's clang + lld against the
# Debian amd64 dev sysroot from scripts/build-x86-sysroot.sh (env X86_SYSROOT).
if (NOT DEFINED ENV{X86_SYSROOT})
  message(FATAL_ERROR "X86_SYSROOT must point at build/rootfs/sysroot-x86_64 (scripts/build-x86-sysroot.sh)")
endif()
set(X86_SYSROOT "$ENV{X86_SYSROOT}")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TARGET_TRIPLE x86_64-linux-gnu)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${TARGET_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${TARGET_TRIPLE})
set(CMAKE_C_COMPILER_AR llvm-ar)
set(CMAKE_CXX_COMPILER_AR llvm-ar)
set(CMAKE_C_COMPILER_RANLIB llvm-ranlib)
set(CMAKE_CXX_COMPILER_RANLIB llvm-ranlib)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_SYSROOT "${X86_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${X86_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must answer from the x86_64 sysroot, not from the build machine and not
# from the arm64 sysroot the parent (host-side) configure uses.
set(ENV{PKG_CONFIG_LIBDIR} "${X86_SYSROOT}/usr/lib/${TARGET_TRIPLE}/pkgconfig:${X86_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${X86_SYSROOT}")
set(ENV{PKG_CONFIG_PATH} "")

# The guest invokers (ThunkLibs/include/common/Guest.h, CallHostFunction) receive the
# host function address in r11 by a custom ABI. With a stack protector, the prologue
# loads the canary into r11 first and the host address is lost (garbage jump in the
# host wrapper; see NOTES.md N-021). Upstream only disables it for 32-bit guests
# because Ubuntu's clang doesn't enable it by default; Arch/CachyOS clang does.
set(CMAKE_C_FLAGS_INIT "-fno-stack-protector")
set(CMAKE_CXX_FLAGS_INIT "-fno-stack-protector")
