#!/usr/bin/env bash
set -euo pipefail

kernel_dir="${1:?usage: install.sh KERNEL_BUILD_COPY}"
integration=$(cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(cd -- "$integration/../.." && pwd)
destination="$kernel_dir/drivers/net/wireless/esp32s31_hosted"
mkdir -p "$destination"
cp "$project_dir"/upstream/esp-hosted-linux/host/*.c "$destination/"
cp -R "$project_dir/upstream/esp-hosted-linux/host/include" "$destination/"
cp "$integration"/*.c "$integration"/*.h "$integration/Kconfig" "$integration/Makefile" \
    "$project_dir/shared/esp32s31-hosted-shm.h" "$destination/"
for patch_file in "$integration"/patches/*.patch; do
    patch --forward --batch --fuzz=0 -p2 -d "$destination" < "$patch_file"
done
grep -q 'source "drivers/net/wireless/esp32s31_hosted/Kconfig"' "$kernel_dir/drivers/net/wireless/Kconfig" || \
    printf '\nsource "drivers/net/wireless/esp32s31_hosted/Kconfig"\n' >> "$kernel_dir/drivers/net/wireless/Kconfig"
grep -q 'CONFIG_ESP32S31_HOSTED.*esp32s31_hosted/' "$kernel_dir/drivers/net/wireless/Makefile" || \
    printf '\nobj-$(CONFIG_ESP32S31_HOSTED) += esp32s31_hosted/\n' >> "$kernel_dir/drivers/net/wireless/Makefile"
