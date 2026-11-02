#!/usr/bin/env bash
# 从 fex-latest release 拉取 FEX 成品（本体 + host/guest thunks + ALSA 运行库），
# 落位 build/fex/install + build/fex/contract.json（范围修订：FEX 构建剥离至
# fex.yml，主管线只消费）。逐资产 sha256 校验；契约字段（fx_package/fxd_root/
# fxd_ldso）与本地 config.sh 不一致立即失败；libc6 方向性校验（fex 的 libc6
# 不新于 Runtime 树的 libc6）在组装前统一执行。
# **没有本地重建回退**（design D5）——缺了就去触发 fex.yml。
#
# Fetches the FEX build product from fex-latest into build/fex/install
# (+ contract.json). sha256 verified per asset; contract fields must match
# scripts/config.sh. NO local rebuild fallback (design D5) — trigger fex.yml instead.
#
# env: NXP_FEX_TAG（默认 fex-latest）、NXP_ROOTFS_REPO（默认本仓库 origin）
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh

TAG="${NXP_FEX_TAG:-fex-latest}"
REPO="${NXP_ROOTFS_REPO:-}"
[[ -n "$REPO" ]] || REPO="$(git remote get-url origin | sed -E 's#.*[:/]([^/]+/[^/.]+)(\.git)?$#\1#')"
DL=build/fex-dl
fail() { echo "FETCH FAIL: $*" >&2; exit 1; }

command -v gh >/dev/null || fail "gh CLI not found"
mkdir -p "$DL"
echo ">> 渠道 ${REPO}@${TAG}：FEX 成品"
gh release download "$TAG" -R "$REPO" -p fex-install.tar.gz -p fex-install.tar.gz.sha256 \
  -p contract.json -D "$DL" \
  || fail "release $TAG not reachable at $REPO — trigger the fex.yml workflow first (Actions → fex + thunks → Run workflow)"

cd "$DL"
sha256sum -c fex-install.tar.gz.sha256 || fail "sha256 mismatch: fex-install.tar.gz"
cd - >/dev/null

mkdir -p build/fex
rm -rf build/fex/install
tar -C build/fex -xzf "$DL/fex-install.tar.gz"           # → install/
[[ -d build/fex/install/data/data ]] || fail "fex-install.tar.gz did not unpack to install/ (layout mismatch)"
cp "$DL/contract.json" build/fex/contract.json
python3 - build/fex/contract.json "$FXD_PACKAGE" "$FXD_ROOT" "$FXD_LDSO" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
checks = [("fx_package", sys.argv[2]), ("fxd_root", sys.argv[3]), ("fxd_ldso", sys.argv[4])]
bad = [(k, c.get(k), want) for k, want in checks if c.get(k) != want]
if bad:
    for k, got, want in bad:
        print(f"CONTRACT MISMATCH: {k}={got!r} but this build expects {want!r}", file=sys.stderr)
    sys.exit(1)
print(f"   fex contract ok (libc6={c.get('libc6')}, tag={c.get('fex_tag')})")
PY
rm -rf "$DL"
echo ">> FEX 就绪：build/fex/install（来源 ${REPO}@${TAG}）"
