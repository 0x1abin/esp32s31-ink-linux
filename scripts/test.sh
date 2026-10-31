#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
compiler="${CC:-cc}"

mkdir -p "$project_dir/outputs/logs"
exec > >(tee "$project_dir/outputs/logs/host-tests.log") 2>&1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/metalio-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT

cd -- "$project_dir"
"$compiler" \
    -std=c11 -Wall -Wextra -Werror -O2 -Iinclude/uapi \
    apps/epd-demo.c -o "$test_dir/metalio-epd-demo"
for fast_refresh in 0 1; do
    "$compiler" \
        -std=c11 -Wall -Wextra -Werror -O2 -Iinclude/uapi \
        -DCONFIG_METALIO_EPD_FAST_REFRESH="$fast_refresh" \
        tests/test_phase.c -o "$test_dir/test-phase-$fast_refresh"
    "$test_dir/test-phase-$fast_refresh"
done
"$compiler" \
    -std=c11 -Wall -Wextra -Werror -O2 \
    tests/test_touch.c -o "$test_dir/test-touch"
"$test_dir/test-touch"
"$compiler" -std=c11 -Wall -Wextra -Werror -O2 \
    tests/test_wifi_boot.c -o "$test_dir/test-wifi-boot"
"$test_dir/test-wifi-boot"
"$compiler" \
    -std=c11 -Wall -Wextra -Werror -O2 -Iinclude/uapi \
    tests/test_terminal_ui.c apps/terminal-ui.c -o "$test_dir/test-terminal-ui"
"$test_dir/test-terminal-ui"
vterm_archive="$project_dir/upstream/downloads/libvterm-0.3.3.tar.gz"
if [[ ! -f "$vterm_archive" ]]; then
    vterm_archive="$project_dir/upstream/downloads/buildroot/libvterm/libvterm-0.3.3.tar.gz"
fi
if [[ ! -f "$vterm_archive" ]]; then
    vterm_archive="${BR2_DL_DIR:-$project_dir/upstream/buildroot/dl}/libvterm/libvterm-0.3.3.tar.gz"
fi
if [[ ! -f "$vterm_archive" ]]; then
    echo 'Missing libvterm source: build libvterm with scripts/build.sh first.' >&2
    exit 1
fi
printf '%s  %s\n' 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0 \
    "$vterm_archive" | sha256sum -c -
tar -xzf "$vterm_archive" -C "$test_dir"
"$compiler" -std=c99 -O2 -fPIC -shared -I"$test_dir/libvterm-0.3.3/include" \
    "$test_dir"/libvterm-0.3.3/src/*.c -o "$test_dir/libvterm.so.0"
"$compiler" -std=c11 -Wall -Wextra -Werror -O2 -Iinclude/uapi \
    -I"$test_dir/libvterm-0.3.3/include" tests/test_terminal.c apps/terminal-ui.c \
    -L"$test_dir" -Wl,-rpath,"$test_dir" -l:libvterm.so.0 -o "$test_dir/test-terminal"
"$test_dir/test-terminal"
env \
    ESP32S31_INK_LINUX_TEST_DEMO="$test_dir/metalio-epd-demo" PYTHONDONTWRITEBYTECODE=1 \
    python3 -B -m unittest discover -s tests -p 'test_*.py' -v
printf 'Host tests passed; no target or physical display was tested.\n'
