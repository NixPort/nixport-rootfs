# nixport-rootfs

Nixport Runtime 组件供给仓库：Debian 树（arm64 Runtime / x86_64 客机 / x86 multilib）、
交叉编译 sysroot（arm64/amd64）、补丁版 glibc/FEX/Mesa。滚动 release `rootfs-latest`
发布，契约见 release 内 `meta.json`（`nixport.rootfs-meta.v1`）。

（完整消费说明随 extract-rootfs-repo change 落地，见 app 仓库 docs/rootfs.md 与
Port 打包规范。）
