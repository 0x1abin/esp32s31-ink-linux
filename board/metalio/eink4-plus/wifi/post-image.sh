#!/usr/bin/env bash
set -euo pipefail
images="${1:?usage: post-image.sh IMAGES_DIR}"
project_dir=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
layout="$project_dir/board/metalio/eink4-plus/wifi/layout.json"
exec python3 "$project_dir/scripts/make-wifi-image.py" \
    --images-dir "$images" --host-dir "${HOST_DIR:?}" --layout "$layout"
