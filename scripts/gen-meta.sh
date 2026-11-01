#!/usr/bin/env bash
# 生成 rootfs-latest release 的契约元数据 build/release/meta.json（spec
# runtime-supply-chain：资产集完整性）。app 仓库据此校验 applicationId 契约路径、
# 读取 libc6 版本（Port 打包符号天花板的依据）、核对资产 sha256。
#
# Builds build/release/meta.json — the contract metadata of the rootfs-latest
# release (spec runtime-supply-chain: asset-set completeness). The app repo
# validates the applicationId contract paths against it, reads the libc6
# version (the Port packaging symbol ceiling), and verifies asset sha256s.
#
# schema: nixport.rootfs-meta.v1
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/config.sh
source scripts/lib/need-artifact.sh

REL=build/release
need_artifact "$REL/rootfs-arm64.tar.gz" scripts/pack-release-assets.sh

SUITE="${SUITE:-trixie}"
PKGS_TXT=build/rootfs/rootfs-arm64.packages.txt
need_artifact "$PKGS_TXT" scripts/build-rootfs-image.sh
LIBC6="$(grep '^libc6 ' "$PKGS_TXT" | awk '{print $2}')"
if [[ -z "$LIBC6" ]]; then
  echo "libc6 entry not found in $PKGS_TXT" >&2; exit 1
fi

GIT_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# meta.json 与逐资产 .sha256 必须同源生成（同一套文件、同一次哈希）。
# meta.json and per-asset .sha256 are generated from the same file set in one pass.
# BASE_META：部分重建时 CI 先取回 release 现存的 meta.json 放到该路径；合并语义
# = 本次打包的资产条目覆盖、其余保留；partial=true 且 rebuilt_from 记本次 sha，
# built_from 保持最近一次完整构建的 sha。
# BASE_META: for partial rebuilds CI puts the release's current meta.json here;
# merge = this run's packed assets overwrite, others are kept; partial=true with
# rebuilt_from = this sha, while built_from stays at the last FULL build's sha.
python3 - "$REL" "$FXD_PACKAGE" "$FXD_ROOT" "$FXD_LDSO" "$LIBC6" "$SUITE" "$GIT_SHA" "$BUILT_AT" "${BASE_META:-}" <<'PY'
import hashlib, json, pathlib, sys

rel = pathlib.Path(sys.argv[1])
base_path = sys.argv[9]
base = {}
if base_path and pathlib.Path(base_path).is_file():
    base = json.loads(pathlib.Path(base_path).read_text())

meta = {
    "schema": "nixport.rootfs-meta.v1",
    "fx_package": sys.argv[2],
    "fxd_root": sys.argv[3],
    "fxd_ldso": sys.argv[4],
    "libc6": sys.argv[5],
    "suite": sys.argv[6],
    "built_from": sys.argv[7],
    "built_at": sys.argv[8],
    "partial": False,
    "assets": {},
}
if base.get("schema") == "nixport.rootfs-meta.v1":
    meta["assets"] = dict(base.get("assets", {}))
    meta["partial"] = True
    meta["built_from"] = base.get("built_from", sys.argv[7])
    meta["rebuilt_from"] = sys.argv[7]

assets = sorted(p for p in rel.iterdir()
                if p.is_file() and (p.suffix in (".gz", ".zst") or p.name.endswith(".txt")))
if not assets and not meta["assets"]:
    sys.exit("no release assets found — run scripts/pack-release-assets.sh first")
for p in assets:
    data = p.read_bytes()
    meta["assets"][p.name] = {
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
    }
    # 逐资产 .sha256：<hash>  <文件名>，供消费方 sha256sum -c
    # per-asset .sha256 for consumer `sha256sum -c`
    (rel / (p.name + ".sha256")).write_text(f"{meta['assets'][p.name]['sha256']}  {p.name}\n")

out = rel / "meta.json"
out.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n")
print(f"meta.json: {len(meta['assets'])} assets ({'partial merge' if meta['partial'] else 'full'}), libc6={meta['libc6']}, suite={meta['suite']}")
PY
cat "$REL/meta.json"
