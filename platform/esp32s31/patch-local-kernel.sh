#!/usr/bin/env bash
set -euo pipefail

kernel_dir="${1:?usage: patch-local-kernel.sh KERNEL_BUILD_COPY}"
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
for patch_file in "$board_dir"/patches/linux/*.patch; do
    if patch --dry-run --forward --batch --fuzz=0 -p1 -d "$kernel_dir" < "$patch_file" >/dev/null 2>&1; then
        patch --forward --batch --fuzz=0 -p1 -d "$kernel_dir" < "$patch_file"
    elif patch --dry-run --reverse --batch --fuzz=0 -p1 -d "$kernel_dir" < "$patch_file" >/dev/null 2>&1; then
        printf 'Already applied: %s\n' "${patch_file##*/}"
    else
        printf 'Cannot apply or verify previously applied patch: %s\n' "$patch_file" >&2
        exit 1
    fi
done
