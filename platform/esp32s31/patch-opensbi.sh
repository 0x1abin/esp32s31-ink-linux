#!/usr/bin/env bash
set -euo pipefail
source_dir="${1:?usage: patch-opensbi.sh OPENSBI_BUILD_COPY}"
patch_dir=$(cd -- "$(dirname -- "$0")/patches/opensbi" && pwd)
for patch_file in "$patch_dir"/*.patch; do
    if patch --dry-run --forward --batch --fuzz=0 -p1 -d "$source_dir" < "$patch_file" >/dev/null 2>&1; then
        patch --forward --batch --fuzz=0 -p1 -d "$source_dir" < "$patch_file"
    elif ! patch --dry-run --reverse --batch --fuzz=0 -p1 -d "$source_dir" < "$patch_file" >/dev/null 2>&1; then
        echo "Cannot apply or verify OpenSBI patch: $patch_file" >&2
        exit 1
    fi
done
