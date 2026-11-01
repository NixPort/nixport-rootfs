#!/usr/bin/env bash
# 缺失前置产物的统一诊断：指名【缺哪个文件】与【由谁产出】。
#
# 构建管线是一条链，每个脚本消费上游脚本的产物。缺失时若只说"请先运行某个脚本"，
# 构建者还得自己去找是哪个文件缺了；下游步骤则更可能报出与病因无关的间接错误
# （例如"cd: build/payload-rootfs: No such file or directory"）。
#
# One diagnostic for missing prerequisites: name BOTH the missing path and the
# script that produces it. Downstream steps are otherwise likely to fail with an
# unrelated symptom, hiding the real cause.
#
# 用法 / usage:
#   source scripts/lib/need-artifact.sh
#   need_artifact build/rootfs/sysroot-arm64/usr/include scripts/build-rootfs.sh
#
# 约定：退出码非 0 即为失败，调用方无需再检查返回值。
# Contract: a non-zero exit means failure; callers need not test the return value.

# need_artifact <路径> <产出该产物的脚本>
# 路径存在则静默通过；缺失则打印诊断并以非零状态退出。
# need_artifact <path> <producing script>
# Silent when the path exists; otherwise prints the diagnostic and exits non-zero.
need_artifact() {
  local path="$1" producer="$2"
  if [[ -e "$path" ]]; then
    return 0
  fi
  {
    echo "missing prerequisite: $path"
    echo "  produced by: $producer"
    echo "  run it first, then retry this step"
  } >&2
  exit 1
}
