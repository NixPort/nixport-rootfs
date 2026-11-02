#!/usr/bin/env bash
# 从 nixport-drivers 拉取**内置默认驱动**（runtime fragment：usr/lib/aarch64-linux-gnu/
# + usr/share/vulkan/icd.d/ 布局），落位 build/mesa/install/$FXD_ROOT 布局供组装。
# 片段的 icd library_path 前缀由消费方 rootfs 决定——assemble-payload 已有改写块，
# 相对路径在设备上按 manifest 目录解析，两者都合法。
# 驱动变体与 tag 由 scripts/driver-pin.env 决定（可复现原则：pin 稳定值，手动升级）。
# **没有本地构建回退**（design D5）——驱动构建的权责在 nixport-drivers。
#
# Fetches the built-in DEFAULT driver (runtime fragment) from nixport-drivers and
# places it into build/mesa/install/<fxd_root layout> for assembly. The fragment's
# icd library_path prefix is consumer-side (assemble already rewrites absolute
# prefixes; relative paths resolve against the manifest dir on device).
# Variant + tag come from scripts/driver-pin.env. NO local build fallback.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh

# shellcheck source=scripts/driver-pin.env
source scripts/driver-pin.env

fail() { echo "FETCH FAIL: $*" >&2; exit 1; }
command -v gh >/dev/null || fail "gh CLI not found"
[[ -n "$DRIVER_ASSET" ]] || fail "DRIVER_ASSET not set in scripts/driver-pin.env — fill it in once the drivers repo CI publishes the runtime fragment"

REPO="${DRIVER_REPO:-NixPort/nixport-drivers}"
DL=build/driver-dl
mkdir -p "$DL"
echo ">> 渠道 ${REPO}@${DRIVER_TAG}：内置驱动 $DRIVER_ASSET（variant=$DRIVER_VARIANT）"
gh release download "$DRIVER_TAG" -R "$REPO" -p "$DRIVER_ASSET" -D "$DL" \
  || fail "asset $DRIVER_ASSET not found on $REPO@$DRIVER_TAG — check driver-pin.env or trigger the drivers CI"

WORK="$DL/work"
mkdir -p "$WORK"
case "$DRIVER_ASSET" in
  *.zip)     unzip -qo "$DL/$DRIVER_ASSET" -d "$WORK" ;;
  *.tar.zst) zstd -dc "$DL/$DRIVER_ASSET" | tar -x -C "$WORK" ;;
  *.tar.gz)  tar -xzf "$DL/$DRIVER_ASSET" -C "$WORK" ;;
  *) fail "unsupported fragment format: $DRIVER_ASSET" ;;
esac

# 片段布局：usr/lib/aarch64-linux-gnu/*.so + usr/share/vulkan/icd.d/*.json
FRAG="$(find "$WORK" -type d -name aarch64-linux-gnu | head -1)"
[[ -n "$FRAG" ]] || fail "fragment layout unexpected (no usr/lib/aarch64-linux-gnu/): $DRIVER_ASSET"
SO="$(find "$(dirname "$FRAG")" -name 'libvulkan_freedreno*.so' | head -1)"
ICD="$(find "$WORK" -path '*vulkan/icd.d*' -name '*.json' | head -1)"
[[ -n "$SO" ]] || fail "no libvulkan_freedreno*.so in the fragment"
[[ -n "$ICD" ]] || fail "no ICD json in the fragment"

# 落位到 assemble 消费的 mesa/install 布局（与 build-mesa.sh 产物同形）
DEST="build/mesa/install$FXD_ROOT"
rm -rf build/mesa/install
mkdir -p "$DEST/usr/lib/$FXD_MULTIARCH" "$DEST$FXD_ROOT/usr/share/vulkan/icd.d"
cp "$SO" "$DEST/usr/lib/$FXD_MULTIARCH/"
cp "$ICD" "$DEST$FXD_ROOT/usr/share/vulkan/icd.d/"
# 单一 ICD 不变式：icd.d 内必须恰好一个 manifest（多驱动走中性目录 + 会话 env，
# 这是 Vulkan loader 语义坑的防线）。
N="$(ls "$DEST$FXD_ROOT/usr/share/vulkan/icd.d"/*.json 2>/dev/null | wc -l)"
[[ "$N" -eq 1 ]] || fail "icd.d must contain exactly 1 json, got $N"

# ICD library_path 规范化：fragment 的前缀由 drivers 侧拼装（实测出现过
# /usr/usr 双写——emit 脚本自拼 /usr/lib 而调用方又传了 /usr 前缀）。这里不猜
# 来源前缀，直接按落位位置重写为设备绝对路径。
# Normalize ICD library_path: the drivers side assembles its own prefix (a
# doubled /usr/usr was observed). Rewrite to the actual placed path.
LIBN="$(basename "$SO")"
python3 - "$DEST$FXD_ROOT/usr/share/vulkan/icd.d/" "$FXD_ROOT/usr/lib/$FXD_MULTIARCH/$LIBN" <<'PY'
import json, sys, pathlib
icd_dir, want = pathlib.Path(sys.argv[1]), sys.argv[2]
for f in icd_dir.glob("*.json"):
    d = json.loads(f.read_text())
    d["ICD"]["library_path"] = want
    f.write_text(json.dumps(d, indent=2) + "\n")
    print(f"icd normalized: {f.name} -> {want}")
PY

# 结构自检：ELF aarch64 + ICD 里能找到 freedreno 字样
file "$DEST/usr/lib/$FXD_MULTIARCH/libvulkan_freedreno.so" | grep -q "ARM aarch64" \
  || fail "driver .so is not aarch64"
grep -q freedreno "$DEST$FXD_ROOT/usr/share/vulkan/icd.d/"*.json || fail "ICD json lacks freedreno entry"

rm -rf "$DL"
echo ">> 默认驱动就绪：build/mesa/install（${REPO}@${DRIVER_TAG} $DRIVER_ASSET，variant=$DRIVER_VARIANT）"
