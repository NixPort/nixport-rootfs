# Shared build configuration, sourced by the build-*.sh scripts.
# 供给仓库（nixport-rootfs）的共享配置：组件构建的路径真源。app 仓库不 source 本
# 文件，而是经 rootfs-latest release 的 meta.json 消费这里的取值并做契约校验。
# Supply repo (nixport-rootfs) shared config: the source of truth for component
# build paths. The app repo does NOT source this file; it consumes the values
# via meta.json in the rootfs-latest release and validates the contract there.
#
# The on-device rootfs path is baked into glibc and every ELF interpreter, so it
# must match the app's package name (single build, see the app repo's
# app/build.gradle.kts). Changing FXD_PACKAGE here changes every published
# component in the next release.
# 设备侧 rootfs 路径烧进 glibc 与每个 ELF interpreter，必须与应用包名一致；
# 改动 FXD_PACKAGE 会作用于下一次发布的全部组件。
FXD_PACKAGE="${FXD_PACKAGE:-ro.cobrabm.fexdroid}"
FXD_ROOT="/data/data/${FXD_PACKAGE}/files/rootfs"
FXD_MULTIARCH="aarch64-linux-gnu"
FXD_LIBDIR="${FXD_ROOT}/usr/lib/${FXD_MULTIARCH}"
FXD_LDSO="${FXD_LIBDIR}/ld-linux-aarch64.so.1"

# 契约内化（extract-rootfs-repo design D4）：补丁版 glibc 的源码版本不再在此维护。
# build-glibc.sh 构建时从 build/rootfs/rootfs-arm64.packages.txt 的 libc6 条目读取，
# 与 Debian 树严格同源；meta.json 如实记录，Port 打包以 meta.json 的 libc6 为
# 符号天花板依据（原 NXP_MAX_GLIBC 由消费方从 meta.json 推导）。
# Contract internalized (extract-rootfs-repo design D4): the patched glibc source
# version is no longer maintained here. build-glibc.sh reads it at build time from
# the libc6 entry of build/rootfs/rootfs-arm64.packages.txt — strictly same-source
# as the Debian tree; meta.json records it, and Port packaging derives its symbol
# ceiling from meta.json's libc6 (NXP_MAX_GLIBC is a consumer-side derivation now).
