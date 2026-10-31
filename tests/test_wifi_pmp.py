"""Check the radio PMP boundary and CSR readback protection."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class WifiPmpTests(unittest.TestCase):
    def test_boot_pmp_keeps_linux_private(self):
        main = (ROOT / "firmware/esp32s31-linux-companion/main/main.c").read_text()
        constants = main[main.index("#define PSRAM_BASE"):main.index('static const char *TAG')]
        constants += "\n".join(re.findall(r"^#define PMP_[^\n]+", main, re.M)) + "\n"
        protection = re.search(r"static bool protect_linux_memory\(void\)\n\{.*?\n\}",
                               main, re.S).group()
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp32s31-hosted-shm.h"
#define METALIO_DTB_MAX_SIZE 65536U
#define SOC_RESERVE_MEMORY_REGION(start, end, name)
#define PMP_L 0x80U
#define PMP_NAPOT 0x18U
#define PMP_TOR 0x08U
#define PMP_R 1U
#define PMP_W 2U
#define PMP_X 4U
/* Match IDF csr.h: NAPOT is unshifted, writes shift by 2, reads shift back. */
#define PMPADDR_NAPOT(start, end) ((start) | (((end) - (start) - 1) >> 1))
static uint32_t address[16], config[16];
static int bad_config_entry = -1, bad_address_entry = -1;
static void write_address(unsigned int i, uint32_t value)
{
    bool tor_lower_locked = i < 15 &&
        (config[i + 1] & (PMP_L | 0x18)) == (PMP_L | PMP_TOR);
    if (!(config[i] & PMP_L) && !tor_lower_locked) address[i] = value;
}
static void write_config(unsigned int i, uint32_t value)
{
    if (!(config[i] & PMP_L)) config[i] = value;
}
static void reset_and_set(unsigned int i, uint32_t addr, uint32_t cfg)
{
    write_address(i, 0); write_config(i, 0);
    write_address(i, addr >> 2); write_config(i, config[i] | cfg);
}
#define PMP_RESET_AND_ENTRY_SET(i, addr, cfg) reset_and_set(i, addr, cfg)
#define PMP_ENTRY_CFG_READ(i) (config[i] ^ (bad_config_entry == (i) ? 1 : 0))
#define PMP_ENTRY_ADDR_READ(i) ((address[i] << 2) ^ (bad_address_entry == (i) ? 4 : 0))
#define ESP_LOGE(...) ((void)0)
''' + constants + protection + r'''
/* Model M-mode PMP's lowest matching entry, partial accesses and locked TOR. */
static bool access_allowed(uint32_t start, unsigned int bytes, unsigned int permission)
{
    uint64_t end = (uint64_t)start + bytes;
    for (unsigned int i = 0; i < 16; i++) {
        uint64_t low, high;
        unsigned int mode = config[i] & 0x18;
        if (!mode) continue;
        if (mode == PMP_TOR) {
            low = (uint64_t)(i ? address[i - 1] : 0) << 2;
            high = (uint64_t)address[i] << 2;
        } else {
            assert(mode == PMP_NAPOT);
            unsigned int ones = 0;
            for (uint32_t bits = address[i]; bits & 1; bits >>= 1) ones++;
            uint64_t size = 1ULL << (ones + 3);
            low = ((uint64_t)address[i] << 2) & ~(size - 1);
            high = low + size;
        }
        if (low >= high || end <= low || start >= high) continue;
        if (start < low || end > high) return false;
        return !(config[i] & PMP_L) || (config[i] & permission) == permission;
    }
    return true;
}
static void assert_rw(uint32_t start, unsigned int bytes, bool allowed)
{
    assert(access_allowed(start, bytes, PMP_R) == allowed);
    assert(access_allowed(start, bytes, PMP_W) == allowed);
}
int main(void)
{
    assert(protect_linux_memory());
    assert(protect_linux_memory()); /* Locked, correctly programmed entries are stable. */
    assert(ESP_SHM_ADDR == 0x2f020000 && ESP_SHM_SIZE == 0xd000);
    assert_rw(ESP_SHM_ADDR, ESP_SHM_SIZE, true);
    assert_rw(FW_PSRAM_HEAP_ADDR, 1, true);
    assert_rw(FW_PSRAM_HEAP_ADDR + FW_PSRAM_HEAP_SIZE - 1, 1, true);
    assert_rw(FW_PSRAM_HEAP_ADDR - 1, 1, false);
    assert_rw(FW_PSRAM_HEAP_ADDR + FW_PSRAM_HEAP_SIZE, 1, false);
    assert_rw(FW_PSRAM_HEAP_ADDR - 1, 2, false);
    assert_rw(FW_PSRAM_HEAP_ADDR + FW_PSRAM_HEAP_SIZE - 1, 2, false);
    assert(!access_allowed(FW_PSRAM_HEAP_ADDR, 4, PMP_X));
    assert_rw(PSRAM_BASE, 1, false);
    assert_rw(PSRAM_BASE + PSRAM_PMA_SIZE - 1, 1, false);
    assert_rw(PSRAM_UNCACHED_BASE, 1, false);
    assert_rw(PSRAM_UNCACHED_BASE + PSRAM_PMA_SIZE - 1, 1, false);
    /* The radio heap's uncached alias cannot bypass Linux isolation. */
    assert_rw(PSRAM_UNCACHED_BASE + FW_PSRAM_HEAP_ADDR - PSRAM_BASE, 4, false);
    assert_rw(DTB_LOAD_ADDR, METALIO_DTB_MAX_SIZE, false);
    assert_rw(OPENSBI_LOAD_ADDR, OPENSBI_LOAD_SIZE, false);
    assert(!access_allowed(DTB_LOAD_ADDR, 4, PMP_X));
    assert(!access_allowed(OPENSBI_LOAD_ADDR, 4, PMP_X));
    assert(LINUX_DMA_POOL_ADDR == 0x2f030000 && LINUX_DMA_POOL_SIZE == 0x40000);
    assert_rw(0x50e00000U, 0x80000U, false);
    assert_rw(LINUX_DMA_POOL_ADDR - 1, 1, true);
    assert_rw(LINUX_DMA_POOL_ADDR, 1, false);
    assert_rw(LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE - 1, 1, false);
    assert_rw(LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE, 1, true);
    assert_rw(LINUX_DMA_POOL_ADDR - 1, 2, false);
    assert_rw(LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE - 1, 2, false);
    assert(!access_allowed(LINUX_DMA_POOL_ADDR, LINUX_DMA_POOL_SIZE, PMP_X));
    /* TOR locks its preceding address even though that entry is OFF. */
    write_address(14, 0);
    assert(address[14] == LINUX_DMA_POOL_ADDR >> 2);
    /* Check every slot's config and address, including OFF TOR lower bounds. */
    for (int i = 10; i < 16; i++) {
        bad_config_entry = i;
        assert(!protect_linux_memory());
        bad_config_entry = -1;
        bad_address_entry = i;
        assert(!protect_linux_memory());
        bad_address_entry = -1;
    }
    assert(protect_linux_memory());
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fixture = directory / "pmp.c"
            fixture.write_text(source)
            executable = directory / "pmp"
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                           ["-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "shared"), str(fixture), "-o", str(executable)],
                           check=True)
            subprocess.run([str(executable)], check=True)
