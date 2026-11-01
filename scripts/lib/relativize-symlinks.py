#!/usr/bin/env python3
"""Rewrites absolute symlinks inside a staged rootfs as relative ones.

On the phone the x86 RootFS lives in a subdirectory, not at "/". FEX resolves
guest paths below it, but several of its host-side lookups (stat, readlink, ...)
use plain *at() calls where an absolute target like /lib/x86_64-linux-gnu/...
would escape into Android's own filesystem. Relative links stay inside.

usage: relativize-symlinks.py <rootfs-dir>
"""
import os, sys

root = os.path.realpath(sys.argv[1])
n = 0
for dirpath, dirnames, filenames in os.walk(root):
    for name in dirnames + filenames:
        p = os.path.join(dirpath, name)
        if not os.path.islink(p):
            continue
        target = os.readlink(p)
        if not target.startswith('/'):
            continue
        rel = os.path.relpath(root + target, dirpath)
        os.unlink(p)
        os.symlink(rel, p)
        n += 1
print(f"relativize-symlinks: {n} absolute symlinks rewritten in {sys.argv[1]}")
