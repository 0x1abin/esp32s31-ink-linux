"""The exact detected Puya part must qualify without blessing other devices."""
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
PATCH = ROOT / "platform/esp32s31/patches/esp-idf/0005-flash-suspend.patch"
SPEC = importlib.util.spec_from_file_location("wifi_sources", ROOT / "scripts/prepare-wifi-sources.py")
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)


class PuyaFlashTests(unittest.TestCase):
    def test_patch_qualifies_exact_id_and_preserves_other_vendors(self):
        upstream = ROOT / "upstream/esp-idf/components/spi_flash"
        if not upstream.is_dir():
            self.skipTest("pinned IDF source is not fetched")
        with tempfile.TemporaryDirectory(prefix="puya-flash-") as tmp:
            tree = Path(tmp)
            component = tree / "components/spi_flash"
            component.mkdir(parents=True)
            for name in ("spi_flash_chip_generic.c", "esp_flash_api.c"):
                (component / name).write_bytes((upstream / name).read_bytes())
            subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-d", str(tree),
                            "-i", str(PATCH)], check=True, stdout=subprocess.DEVNULL)
            source = (component / "spi_flash_chip_generic.c").read_text()
            caps = re.search(r"spi_flash_caps_t spi_flash_chip_generic_get_caps\(.*?\n\}", source, re.S).group()
            setup = re.search(r"esp_err_t spi_flash_chip_generic_suspend_cmd_conf\(.*?\n\}", source, re.S).group()
            code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef int esp_err_t;
typedef unsigned int spi_flash_caps_t;
#define SPI_FLASH_CHIP_CAP_SUSPEND 1U
#define SPI_FLASH_CHIP_CAP_32MB_SUPPORT 2U
#define SPI_FLASH_CHIP_CAP_UNIQUE_ID 4U
#define CMD_RDSFDP 0x5a
#define CMD_RDSR2 0x35
#define CMD_SUSPEND 0x75
#define CMD_RESUME 0x7a
typedef struct { unsigned int sus_mask, cmd_rdsr, sus_cmd, res_cmd; } spi_flash_sus_cmd_conf;
typedef struct { unsigned int command, address_bitlen, address, mosi_len, dummy_bitlen;
    void *mosi_data, *miso_data; unsigned int miso_len; } spi_flash_trans_t;
struct host;
struct operations {
    esp_err_t (*common_command)(struct host *, spi_flash_trans_t *);
    esp_err_t (*sus_setup)(struct host *, const spi_flash_sus_cmd_conf *);
};
struct host { struct operations *driver; };
typedef struct { uint32_t chip_id; struct host *host; } esp_flash_t;
static unsigned char sfdp;
static esp_err_t read_sfdp(struct host *host, spi_flash_trans_t *t) {
    assert(t->command == 0x5a && t->address == 0x32 && t->address_bitlen == 24);
    *(unsigned char *)t->miso_data = sfdp; return 0;
}
static esp_err_t configure(struct host *host, const spi_flash_sus_cmd_conf *c) {
    assert(c->sus_mask == 0x80 && c->cmd_rdsr == 0x35);
    assert(c->sus_cmd == 0x75 && c->res_cmd == 0x7a); return 0;
}
''' + caps + setup + r'''
int main(void) {
    struct operations ops = {read_sfdp, configure}; struct host host = {&ops};
    esp_flash_t chip = {.chip_id=0x852018, .host=&host};
    bool qualified = CONFIG_IDF_TARGET_ESP32S31 && CONFIG_SPI_FLASH_AUTO_SUSPEND &&
        CONFIG_SPI_FLASH_SUSPEND_TSUS_VAL_US >= 30 && CONFIG_SPI_FLASH_SUSPEND_TRS_VAL_US >= 100;
    assert(!!(spi_flash_chip_generic_get_caps(&chip) & SPI_FLASH_CHIP_CAP_SUSPEND) == qualified);
    assert(!spi_flash_chip_generic_suspend_cmd_conf(&chip));
    const uint32_t rejected[] = {0x852017,0x852019,0x856018,0x852318,0xef4018,0x123456};
    for (unsigned int n=0;n<sizeof(rejected)/sizeof(rejected[0]);n++) {
        chip.chip_id=rejected[n];
        assert(!(spi_flash_chip_generic_get_caps(&chip) & SPI_FLASH_CHIP_CAP_SUSPEND));
    }
    chip.chip_id=0x462018; assert(spi_flash_chip_generic_get_caps(&chip) & SPI_FLASH_CHIP_CAP_SUSPEND);
    chip.chip_id=0xa14018; assert(spi_flash_chip_generic_get_caps(&chip) & SPI_FLASH_CHIP_CAP_SUSPEND);
    chip.chip_id=0x204018; sfdp=0; assert(!(spi_flash_chip_generic_get_caps(&chip)&SPI_FLASH_CHIP_CAP_SUSPEND));
    sfdp=8; assert(spi_flash_chip_generic_get_caps(&chip)&SPI_FLASH_CHIP_CAP_SUSPEND);
}
'''
            c_file, binary = tree / "test.c", tree / "test"
            c_file.write_text(code)
            for target, suspend, tsus, trs in ((1,1,50,100),(1,1,29,100),(1,1,50,99),
                                               (1,0,50,100),(0,1,50,100)):
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-Wno-unused-parameter", f"-DCONFIG_IDF_TARGET_ESP32S31={target}",
                                f"-DCONFIG_SPI_FLASH_AUTO_SUSPEND={suspend}",
                                f"-DCONFIG_SPI_FLASH_SUSPEND_TSUS_VAL_US={tsus}",
                                f"-DCONFIG_SPI_FLASH_SUSPEND_TRS_VAL_US={trs}",
                                str(c_file), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
            api = (component / "esp_flash_api.c").read_text()
            self.assertIn('"Suspend/resume unsupported: JEDEC=0x%06" PRIx32', api)
            self.assertIn('return ESP_ERR_NOT_SUPPORTED;', api)

    def test_project_patch_participates_in_owned_source_fingerprint(self):
        with mock.patch.object(PREPARE, "prepare") as prepare:
            PREPARE.main()
        self.assertIn(PATCH, prepare.call_args_list[0].args[1])
        config = (ROOT / "board/metalio/eink4-plus/wifi/sdkconfig.defaults").read_text().splitlines()
        self.assertIn("CONFIG_SPI_FLASH_AUTO_SUSPEND=y", config)
        self.assertIn("CONFIG_SPI_FLASH_SUSPEND_TSUS_VAL_US=50", config)
        self.assertIn("CONFIG_SPI_FLASH_SUSPEND_TRS_VAL_US=100", config)


if __name__ == "__main__":
    unittest.main()
