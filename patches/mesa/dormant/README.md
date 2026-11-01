# 备用补丁（不参与 build-mesa.sh 的 patches/mesa/*.patch 应用集）

## 0008-tu-reject-end-rendering-without-state.patch

来源：gow_android_glibc（E.1 渲染范围跨 command buffer 代配对修复的驱动侧兜底）。
状态：**备用不启用**——host 侧护栏（runtime 的 vk_scheduler 配对护栏）已真机验证足够；
启用前提：真机再现 Turnip `tu_emit_custom_resolve_end` 空 `state.subpass` SIGSEGV
（8gen3 `0x4e` 崩溃时序），且 host 护栏确认失效。

启用方式：移出 dormant/ 至 patches/mesa/ 并按 fexdroid Mesa pin 重新
`git apply --check`（原基于 Mesa main 35b085c4；不匹配时先 rebase）。
证据：gow_android_glibc my-docs/08-Android移植/渲染范围跨代配对-取证与修复-20261023.md
