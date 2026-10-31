"""Host checks for CPU0 Flash operations and bounded cache coordination."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware/esp32s31-linux-companion/components/esp_hosted"

class WifiFirmwareTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory(prefix="cpu0-fw-", dir=os.environ.get("TMPDIR")) as tmp:
            source = Path(tmp) / "check.c"
            binary = Path(tmp) / "check"
            source.write_text(code)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-I", str(FW), "-I", str(ROOT / "kernel"),
                            "-I", str(ROOT / "shared"), "-I", str(ROOT / "include/uapi"),
                            str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_flash_and_cache_propagate_timeouts(self):
        source = (FW / "shm_flash.c").read_text()
        flash = re.search(r"static esp_err_t flash_op\(.*?\n\}", source, re.S).group()
        lock = re.search(r"static int IRAM_ATTR cache_locked\(.*?\n\}", source, re.S).group()
        self.compile_run(r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#define SHM_CACHE_TIMEOUT_MS 100U
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 7
#define ESP_SHM_ADDR 0x2f020000U
#define ESP_SHM_FLASH_DATA 0x800
#define ESP_SHM_FLASH_PROGRAM 2
#define ESP_SHM_FLASH_ERASE 3
#define ESP_SHM_FLASH_SECTOR 4096
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA 16
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define IRAM_ATTR
static bool s_cache_timeout, s_cache_shared, semaphore_available = true, inject_timeout;
static uint32_t s_base, cache_lock;
static void *s_flash_lock = &cache_lock;
static unsigned int acquired, released, operation_count, irq_masked;
static int64_t now;
#define CACHE_LOCK (&cache_lock)
static int xSemaphoreTake(void *lock, unsigned int ticks) { acquired++; return semaphore_available; }
static void xSemaphoreGive(void *lock) { released++; }
static int64_t esp_timer_get_time(void) { now += 10000; return now; }
static unsigned int portSET_INTERRUPT_MASK_FROM_ISR(void) { irq_masked = 1; return 7; }
static void portCLEAR_INTERRUPT_MASK_FROM_ISR(unsigned int value) { assert(value == 7); irq_masked = 0; }
static int cache_operation(uint32_t map, uint32_t address, uint32_t bytes) { operation_count++; return 0; }
static esp_err_t esp_flash_write(void *chip, const void *data, uint32_t address, uint32_t bytes) {
    assert(!s_cache_timeout); if (inject_timeout) s_cache_timeout = true; return ESP_OK;
}
static esp_err_t esp_flash_erase_region(void *chip, uint32_t address, uint32_t bytes) {
    assert(!s_cache_timeout); if (inject_timeout) s_cache_timeout = true; return ESP_OK;
}
''' + flash + lock + r'''
int main(void) {
    assert(!flash_op(2,0,128)); assert(!flash_op(3,0,65536));
    inject_timeout = true;
    assert(flash_op(2,0,128) == ESP_ERR_TIMEOUT);
    assert(flash_op(3,0,65536) == ESP_ERR_TIMEOUT);
    inject_timeout = false; semaphore_available = false;
    unsigned int releases = released;
    assert(flash_op(2,0,128) == ESP_ERR_TIMEOUT);
    assert(released == releases);
    assert(!cache_locked(cache_operation,16,0x50e34000,64));
    assert(operation_count == 1 && !irq_masked);
    s_cache_shared = true; cache_lock = 0; s_cache_timeout = false;
    assert(!cache_locked(cache_operation,16,0x50e34000,64));
    assert(operation_count == 2 && !cache_lock && !irq_masked);
    cache_lock = 1; now = 0;
    assert(cache_locked(cache_operation,16,0x50e34000,64) == 1);
    assert(s_cache_timeout && cache_lock == 1 && operation_count == 2 && !irq_masked);
    assert(now >= SHM_CACHE_TIMEOUT_MS * 1000);
}
''')

if __name__ == "__main__":
    unittest.main()
