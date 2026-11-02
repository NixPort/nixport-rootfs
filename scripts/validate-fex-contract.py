#!/usr/bin/env python3
"""fex-latest 契约校验（fx_package/fxd_* 一致 + libc6 方向性）。

方向性规则：fex 编译所用 libc6 的上游版本 MUST NOT 新于 Runtime 树的 libc6
（旧符号跑在新 glibc 上永远合法；反向 = 运行时符号缺失）。同上游不同修订视为
ABI 兼容（glibc 修订版本不破坏符号版本），仅提示。
"""
import json
import sys


def upstream(v: str) -> str:
    return v.split("-")[0]


def main() -> None:
    contract_path, manifest_path = sys.argv[1], sys.argv[2]
    c = json.load(open(contract_path))
    for key in ("fx_package", "fxd_root", "fxd_ldso"):
        if not c.get(key):
            sys.exit(f"FEX CONTRACT: missing {key}")
    tree = next(
        line.split()[1] for line in open(manifest_path) if line.startswith("libc6 ")
    )
    fu, tu = upstream(c["libc6"]), upstream(tree)
    if fu > tu:
        sys.exit(
            f"LIBC6 REGRESSION: fex built against libc6={c['libc6']} but the runtime "
            f"tree has {tree} — re-run fex.yml against the current tree"
        )
    if c["libc6"] != tree:
        print(f"   note: fex libc6 revision differs from tree ({c['libc6']} vs {tree}) — ABI compatible, ok")
    print(f"   fex contract ok (libc6={c['libc6']}, tag={c.get('fex_tag')})")


if __name__ == "__main__":
    main()
