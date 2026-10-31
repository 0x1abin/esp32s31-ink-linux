#!/usr/bin/env bash
set -euo pipefail
unset LDFLAGS
export KBUILD_BUILD_USER=esp32s31-ink-linux
export KBUILD_BUILD_HOST=builder
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$project_dir/scripts/fetch-sources.py" --wifi --verify
python3 "$project_dir/scripts/prepare-wifi-sources.py"
build_dir="${ESP32S31_INK_LINUX_WIFI_BUILD_DIR:-$project_dir/outputs/buildroot-eink4-plus-wifi}"
mkdir -p "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
args=(-C "$project_dir/upstream/buildroot" "O=$build_dir" "BR2_EXTERNAL=$project_dir")
if [[ ! -f "$build_dir/.config" ]]; then
    make "${args[@]}" metalio_eink4_plus_wifi_defconfig
fi
grep -qx 'BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y' "$build_dir/.config" || {
    echo 'Stale build configuration; choose a clean esp32s31-ink-linux Wi-Fi build directory.' >&2
    exit 1
}
grep -qx 'BR2_PACKAGE_ESP32S31_LINUX_COMPANION=y' "$build_dir/.config" || {
    echo 'The selected build directory is not an ESP32-S31 Wi-Fi configuration.' >&2
    exit 1
}
if grep -qx 'BR2_PACKAGE_METALIO_CPU0_IO=y' "$build_dir/.config"; then
    echo 'Stale experimental configuration; choose a clean Wi-Fi output directory.' >&2
    exit 1
fi
exec make "${args[@]}" "$@"
