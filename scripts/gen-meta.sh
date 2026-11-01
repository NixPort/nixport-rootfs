#!/usr/bin/env bash
# 生成渠道级契约元数据：<channel-dir>/meta.json（schema nixport.rootfs-meta.v1）。
# 范围修订（多渠道 release）：每个渠道（payload/sysroot/rootfs/multilib）是独立的
# 滚动 release，各自携带自己的 meta——契约字段全局一致（FXD_PACKAGE/FXD_ROOT/
# FXD_LDSO/libc6），assets 段只含本渠道资产。渠道自足 → 不再有跨渠道 BASE_META
# 合并（多渠道拆分前的复杂度）。
#
# Generates per-channel contract metadata: <channel-dir>/meta.json. Each channel
# (payload/sysroot/rootfs/multilib) is its own rolling release carrying its own
# meta — global contract fields + that channel's asset entries only. Channels are
# self-contained, so the old cross-channel BASE_META merge is gone.
#
# 用法 / usage: gen-meta.sh <payload|sysroot|rootfs|multilib>
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh

CH="${1:?usage: gen-meta.sh <payload|sysroot|rootfs|multilib>}"
REL="build/release/$CH"
[[ -d "$REL" && -n "$(ls "$REL" 2>/dev/null)" ]] || { echo "channel dir empty or missing: $REL (pack first)" >&2; exit 1; }

MANIFEST=build/rootfs/rootfs-arm64.packages.txt
if [[ ! -f "$MANIFEST" ]]; then
  # multilib 流水线只产 multilib 树：libc6 从 arch 限定的清单取。
  # The multilib workflow only builds the multilib tree; libc6 from the arch-qualified manifest.
  MANIFEST=build/rootfs/rootfs-x86-multilib.packages.txt
  LIBC6="$(grep -m1 '^libc6[: ]' "$MANIFEST" | awk '{print $2}')"
else
  LIBC6="$(grep '^libc6 ' "$MANIFEST" | awk '{print $2}')"
fi
[[ -n "$LIBC6" ]] || { echo "libc6 entry not found in $MANIFEST" >&2; exit 1; }

GIT_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

python3 - "$REL" "$CH" "$FXD_PACKAGE" "$FXD_ROOT" "$FXD_LDSO" "$LIBC6" "${SUITE:-trixie}" "$GIT_SHA" "$BUILT_AT" <<'PY'
import hashlib, json, pathlib, sys

rel = pathlib.Path(sys.argv[1])
ch = sys.argv[2]
meta = {
    "schema": "nixport.rootfs-meta.v1",
    "channel": ch,
    "fx_package": sys.argv[3],
    "fxd_root": sys.argv[4],
    "fxd_ldso": sys.argv[5],
    "libc6": sys.argv[6],
    "suite": sys.argv[7],
    "built_from": sys.argv[8],
    "built_at": sys.argv[9],
    "assets": {},
}
assets = sorted(p for p in rel.iterdir()
                if p.is_file() and (p.name.endswith((".gz", ".zst", ".txt"))
                                    or p.name in ("payload.version", "payload.json")))
if not assets:
    sys.exit(f"no assets in {rel}")
for p in assets:
    data = p.read_bytes()
    meta["assets"][p.name] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
    # 逐资产 .sha256：<hash>  <文件名>，供消费方 sha256sum -c
    (rel / (p.name + ".sha256")).write_text(f"{meta['assets'][p.name]['sha256']}  {p.name}\n")
out = rel / "meta.json"
out.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n")
print(f"meta.json[{ch}]: {len(meta['assets'])} assets, libc6={meta['libc6']}, suite={meta['suite']}")
PY
cat "$REL/meta.json"
