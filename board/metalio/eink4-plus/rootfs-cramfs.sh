#!/usr/bin/env bash
set -euo pipefail

target="${1:?usage: rootfs-cramfs.sh TARGET_DIR}"
"${HOST_DIR:?}/bin/mkcramfs" -L -q -n metalio-rootfs "$target" "${BINARIES_DIR:?}/rootfs.cramfs"
size=$(stat -c%s "$BINARIES_DIR/rootfs.cramfs")
if ((size > 0x400000)); then
    echo "rootfs.cramfs exceeds the 4 MiB NOR slot ($size bytes)" >&2
    exit 1
fi
