#!/usr/bin/env bash
set -euo pipefail

images="${1:?usage: post-image.sh IMAGES_DIR}"
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
# The pinned vendor packager validates every slot after wrapping the SPL.
[[ -s "$images/rootfs.cramfs" ]] || { echo "Missing rootfs.cramfs" >&2; exit 1; }
exec "${HOST_DIR:?}/share/esp-linux-bsp/tools/gen_esp_flash_image.sh" \
    "$board_dir/esp32s31-layout.cfg" "$images"
