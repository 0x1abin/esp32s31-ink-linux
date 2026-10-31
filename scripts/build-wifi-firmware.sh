#!/usr/bin/env bash
set -euo pipefail
project_dir="${ESP32S31_INK_LINUX_PROJECT_DIR:-$(cd -- "$(dirname -- "$0")/.." && pwd)}"
source_dir="${1:?usage: build-wifi-firmware.sh PROJECT_COPY BUILD_DIR BOARD_CONFIG_DIR}"
build_dir="${2:?usage: build-wifi-firmware.sh PROJECT_COPY BUILD_DIR BOARD_CONFIG_DIR}"
board_dir="${3:?usage: build-wifi-firmware.sh PROJECT_COPY BUILD_DIR BOARD_CONFIG_DIR}"
source_dir=$(cd -- "$source_dir" && pwd)
board_dir=$(cd -- "$board_dir" && pwd)
mkdir -p "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
if [[ -f "$build_dir/sdkconfig" ]] && grep -qx 'CONFIG_METALIO_CPU0_IO=y' "$build_dir/sdkconfig"; then
    echo 'Stale experimental firmware; choose a clean build directory.' >&2
    exit 1
fi
for config in sdkconfig.defaults partitions.csv; do
    [[ -s "$board_dir/$config" ]] || { echo "Missing board configuration: $config" >&2; exit 1; }
    cp -- "$board_dir/$config" "$source_dir/$config"
done
sdkconfig_defaults="$source_dir/sdkconfig.defaults"
export IDF_PATH="$project_dir/outputs/esp32s31-ink-linux-sources/esp-idf"
export ESP32S31_INK_LINUX_PROJECT_DIR="$project_dir"
export ESP32S31_INK_LINUX_HOSTED_PATH="$project_dir/outputs/esp32s31-ink-linux-sources/esp-hosted-linux"
export IDF_COMPONENT_MANAGER=0
export IDF_SKIP_CHECK_SUBMODULES=1
export CMAKE_GENERATOR=Ninja
export IDF_TOOLS_PATH="${ESP32S31_INK_LINUX_IDF_TOOLS_PATH:-$project_dir/upstream/idf-tools}"
mkdir -p "$IDF_TOOLS_PATH"
python_version=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
python="${ESP32S31_INK_LINUX_IDF_PYTHON:-$IDF_TOOLS_PATH/python_env/idf6.1_py${python_version}_env/bin/python}"
tool_bin="${ESP32S31_INK_LINUX_IDF_TOOLCHAIN_BIN:-$IDF_TOOLS_PATH/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin}"
if [[ ! -x "$python" || ! -x "$tool_bin/riscv32-esp-elf-gcc" ]]; then
    echo 'Missing ESP-IDF 6.1 environment; follow docs/wifi.md to install the pinned tools.' >&2
    exit 1
fi
export IDF_PYTHON_ENV_PATH="$(dirname -- "$(dirname -- "$python")")"
export PATH="$tool_bin:$(dirname -- "$python"):$PATH"
if [[ -d "$IDF_TOOLS_PATH/tools/esp-rom-elfs/20241011" ]]; then
    export ESP_ROM_ELF_DIR="$IDF_TOOLS_PATH/tools/esp-rom-elfs/20241011"
fi
exec "$python" "$IDF_PATH/tools/idf.py" --no-hints -C "$source_dir" -B "$build_dir" \
    -D "SDKCONFIG=$build_dir/sdkconfig" -D "SDKCONFIG_DEFAULTS=$sdkconfig_defaults" \
    -D IDF_TARGET=esp32s31 build
