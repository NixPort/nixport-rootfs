#!/usr/bin/env python3
"""Rewrites ELF files in a staged rootfs for the on-device layout.

- PT_INTERP /lib/ld-linux-aarch64.so.1 -> the rootfs loader (absolute Android path)
- absolute DT_RUNPATH/DT_RPATH entries (/usr/lib/...) -> prefixed with the rootfs

Usage: fix-elf.py <stage-dir> <device-root> <device-ldso>
"""
import os, subprocess, sys

stage, root, ldso = sys.argv[1:4]
STD_INTERPS = {"/lib/ld-linux-aarch64.so.1", "/usr/lib/ld-linux-aarch64.so.1",
               "/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
               "/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"}

def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False

def patchelf(*args):
    r = subprocess.run(["patchelf", *args], capture_output=True, text=True)
    return r.returncode, r.stdout.strip()

n_interp = n_rpath = n_elf = 0
for dirpath, _, files in os.walk(stage):
    for name in files:
        p = os.path.join(dirpath, name)
        if os.path.islink(p) or not is_elf(p):
            continue
        n_elf += 1
        rc, interp = patchelf("--print-interpreter", p)
        if rc == 0 and interp in STD_INTERPS:
            mode = os.stat(p).st_mode
            os.chmod(p, mode | 0o200)
            if patchelf("--set-interpreter", ldso, p)[0] != 0:
                sys.exit(f"set-interpreter failed: {p}")
            os.chmod(p, mode)
            n_interp += 1
        rc, rpath = patchelf("--print-rpath", p)
        if rc == 0 and rpath:
            parts = rpath.split(":")
            fixed = [root + x if x.startswith("/") and not x.startswith(root) else x for x in parts]
            if fixed != parts:
                mode = os.stat(p).st_mode
                os.chmod(p, mode | 0o200)
                patchelf("--set-rpath", ":".join(fixed), p)
                os.chmod(p, mode)
                n_rpath += 1

print(f"fix-elf: {n_elf} ELF files, {n_interp} interpreters and {n_rpath} rpaths rewritten")
