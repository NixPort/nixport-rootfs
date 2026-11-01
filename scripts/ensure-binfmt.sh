#!/usr/bin/env bash
# Register qemu-aarch64 binfmt via Docker if missing (not persistent across reboots).
# Avoids needing the host qemu-user-static package.
set -euo pipefail
if [[ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]]; then
  docker run --privileged --rm tonistiigi/binfmt --install arm64 >/dev/null
fi
docker run --rm --platform linux/arm64 debian:trixie-slim uname -m | grep -qx aarch64
echo "binfmt arm64: ok"
