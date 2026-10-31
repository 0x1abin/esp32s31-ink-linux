# Wi-Fi implementation: sources, licenses and modifications

The optional Wi-Fi build combines Linux on CPU1 with ESP-IDF/ESP-Hosted on CPU0.
Each component keeps its original license; the project default does not
relicense the firmware, the shared header, or third-party radio libraries.
Exact revisions are pinned in [sources.lock.json](../sources.lock.json).

| Incorporated source | Origin and license |
| --- | --- |
| CPU0 boot/loader in `firmware/esp32s31-linux-companion/main/main.c` and firmware build files | Marco Müller, [annoyedmilk/esp32-s31-linux](https://github.com/annoyedmilk/esp32-s31-linux/tree/2f3eb1268edd1bb2ca002eb4be339a635003b1e4/bootloader), revision `2f3eb1268edd1bb2ca002eb4be339a635003b1e4`; file-level BSD-2-Clause and retained author notices |
| Shared-memory network and flash drivers under `kernel/esp32s31-hosted/` | Same reference revision, `linux/drivers/net/wireless/espressif/esp_hosted/`; GPL-2.0-only |
| Firmware shared-memory transport and flash mailbox under `firmware/esp32s31-linux-companion/components/esp_hosted/` | Same reference revision, `bootloader/components/esp_hosted/`; Apache-2.0 |
| `shared/esp32s31-hosted-shm.h` | Same reference revision; GPL-2.0-only OR Apache-2.0, as marked in the header |
| OpenSBI platform under `platform/esp32s31/opensbi/` | Same reference revision, `opensbi/platform/esp32s31/`; BSD-2-Clause and retained Marco Müller author notice |
| ESP-Hosted Linux driver core, copied into the kernel build | [espressif/esp-hosted-linux](https://github.com/espressif/esp-hosted-linux/tree/5b717715613527d005fb7ea1ae23c8f73d764271), revision `5b717715613527d005fb7ea1ae23c8f73d764271`; GPL-2.0 and individual file notices |
| ESP-Hosted firmware network adapter | Same ESP-Hosted revision, `esp/esp_driver/network_adapter/`; Apache-2.0 |
| ESP-IDF and its radio/PHY/coexistence binary archives | [espressif/esp-idf](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643), revision `fff9895c82d744c7237be8847347bdd1b07c6643`; Apache-2.0 and component-specific notices, including the licenses shipped beside each binary archive |
| WPA supplicant, libnl, BusyBox, Linux, OpenSBI and toolchain runtimes | Upstream package licenses recorded by Buildroot and the component notices included in the release |

The reference repository's top-level license is MIT, with copyright
`2026 Marco Müller <hello@annoyedmilk.ch>`. Its explicit file-level BSD,
GPL and Apache identifiers take precedence for the incorporated files above.
The release preserves the reference MIT notice, OpenSBI BSD-2-Clause text,
Linux GPL text, ESP-IDF/ESP-Hosted Apache text, and radio-library notices.

The project changes the reference loader, mappings, CPU/interrupt/timer setup
and flash mailbox to match this board and its fixed flash layout. It retains
Linux display/touch ownership, keeps display phase data and descriptors in
the coherent SRAM pool, and restricts writable MTD access to the final 1 MiB
data partition. New validation, timeout and configuration code retains the source
file's stated license. The shared ABI header keeps its dual license.

## Patches and corresponding sources

- `kernel/esp32s31-hosted/patches/0001-metalio-hosted-wifi-integration.patch` consolidates
  the pinned reference's workqueue, regulatory, disconnect, debugfs, scan and
  TX-framing fixes with the project's Wi-Fi-only integration. Original patch
  author and sign-off lines are retained in its header.
- `platform/esp32s31/patches/opensbi/` adapts the reference's supervisor CLIC
  trap/timer/IPI support to the pinned OpenSBI. The platform and original
  OpenSBI file notices remain applicable.
- `platform/esp32s31/patches/linux-companion/` contains the corresponding Linux
  CPU1/interrupt/trap changes; patch headers identify original authors and
  source files retain their Linux licenses.
- `patches/wpa_supplicant/` permits non-terminal password input
  in the pinned 2.11 `wpa_passphrase`, allowing private stdin delivery instead
  of a password argument. Jouni Malinen's original BSD license and copyright
  notice remain in the patched source.
- `scripts/prepare-wifi-sources.py` applies five project-owned IDF patch topics:
  ROM bindings, MSPI underflow, PHY clock/state, timer dispatch and Flash suspend.
  They consolidate ESP-Hosted's `rom.patch`, the pinned reference's IDF fixes
  (excluding Bluetooth), and the board Flash fix. Headers retain original
  attribution; patched files retain their upstream licenses.

The release includes the project sources and patches, applied IDF patches,
actual configurations, prepared-source fingerprints and pinned upstream
revisions. Buildroot `legal-info` collects package notices and available
source archives; keep the corresponding pinned Linux, ESP-IDF, ESP-Hosted,
OpenSBI, reference, toolchain and package sources and all local changes when
redistributing a binary. ESP-IDF binary archives are not project-authored
source code; preserve their shipped license terms separately.

Build checks and license collection do not establish operation or regulatory
qualification of this hardware/firmware combination.
