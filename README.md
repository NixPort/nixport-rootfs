# nixport-rootfs

Nixport 的 **Runtime 组件供给仓库**：Debian 树（arm64 Runtime / x86_64 客机 /
x86 multilib）、交叉编译 sysroot（arm64/amd64）、补丁版 glibc / FEX / Mesa。
单条流水线（`.github/workflows/supply.yml`）构建 → 发布前自检 → 发布到滚动 release
**`rootfs-latest`**，app 仓库（fexdroid）与 Port 开发者从这里消费。

> 目录与树的语义（谁跑在哪、rootfs 和 sysroot 的区别）：
> app 仓库 `my-docs/运行环境/rootfs与sysroot-四棵树区分指南.md`。

## Release 资产与契约

每个资产是**单条目 tar.gz**：解包到消费方的 `build/` 直接得到与本地构建一致的布局。
`meta.json`（schema `nixport.rootfs-meta.v1`）是契约：`fx_package` / `fxd_root` /
`fxd_ldso`（烧进每个 ELF 的设备路径）、`libc6`（补丁 glibc 的源版本 = Port 打包
符号天花板）、逐资产 `sha256`/`size`、`built_from`/`built_at`；部分重建时
`partial=true` + `rebuilt_from`。逐资产 `.sha256` 供 `sha256sum -c`。

| 资产 | 消费方 | 解包到 |
|---|---|---|
| `rootfs-arm64.tar.gz` | app release 组装 | `build/rootfs/rootfs-arm64.tar` |
| `rootfs-x86_64.tar.gz` | app release 组装 | `build/rootfs/rootfs-x86_64.tar` |
| `sysroot-arm64.tar.gz` | app 组装 + **Port 开发者交叉编译** | `build/rootfs/sysroot-arm64/` |
| `sysroot-x86_64.tar.gz` | app 组装（FEX thunks 构建期输入） | `build/rootfs/sysroot-x86_64/` |
| `glibc/fex/mesa-install.tar.gz` | app release 组装 | `build/<组件>/install/` |
| `rootfs-x86-multilib.tar.gz` | **手机端运行时下载**（X86Rootfs.kt） | app 内安装 |
| `rootfs-arm64.tar.zst` (+uris/base) | 设备直装 / `nxp-rootfs-topup.sh` | 手机 |

## 构建（流水线）

```bash
scripts/build-rootfs-image.sh            # Runtime 树（锁定 uris + base digest）
SYSROOT_ONLY=1 scripts/build-rootfs.sh   # sysroot-arm64 + x86_64 客机树
scripts/build-glibc.sh                   # 补丁版 glibc（源版本自动取 packages.txt 的 libc6）
scripts/build-fex.sh                     # FEX + thunks
scripts/build-mesa.sh                    # Mesa Turnip (KGSL)
scripts/build-rootfs-x86-multilib.sh     # x86 multilib 客机树
scripts/pack-release-assets.sh           # → build/release/（逐资产 .sha256）
scripts/smoke-test.sh                    # 发布前自检（失败 MUST NOT 发布）
scripts/gen-meta.sh                      # → build/release/meta.json
```

CI 触发：`paths`（scripts/patches/third_party）push 全量重建；
`workflow_dispatch` 可选 `SUITE`（trixie/sid）、`SNAPSHOT`（Debian 快照 pin）、
`rebuild` 选择器（glibc/fex/mesa/sysroots/multilib 单独重建——跳过的资产保留
release 上旧版，meta 合并）。`concurrency` 串行化消灭 clobber 竞态。

## 改组件补丁怎么调试（不触 CI、不触 Gradle）

```bash
# 1. 在本仓库改 patches/{glibc,fex,mesa}/…（重放工具在 scripts/lib/make-*.py）
scripts/build-glibc.sh                    # 2. 本地重建该组件（docker，源有缓存）
# 3. app 仓库本地组装，组件指向本仓库工作目录：
cd ../fexdroid
NXP_COMPONENTS_DIR=../nixport-rootfs/build scripts/build-payload.sh
scripts/push-rootfs.sh                    # 4. 推真机验证
# 5. 满意后 commit → CI 发布 latest → app release 拉到
```

## Port 开发者：拿 sysroot 交叉编译

```bash
gh release download rootfs-latest -R NixPort/nixport-rootfs \
  -p sysroot-arm64.tar.gz -p sysroot-arm64.tar.gz.sha256
sha256sum -c sysroot-arm64.tar.gz.sha256
tar -C ~/nixport-sysroot -xzf sysroot-arm64.tar.gz
clang --target=aarch64-linux-gnu --sysroot=~/nixport-sysroot/sysroot-arm64 ...
```

glibc 符号天花板 = `meta.json` 的 `libc6` 主版本（即 app 侧 `NXP_MAX_GLIBC`）；
打包纪律见 app 仓库 `my-docs/移植规范/Port打包规范.md`。

## 许可

patches 与本仓库脚本遵循 app 仓库 LICENSES.md 的同等条款；`third_party/aosp-bionic-seccomp`
为 AOSP（Apache-2.0，见目录内 LICENSE）。
