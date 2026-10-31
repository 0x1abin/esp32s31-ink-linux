"""Check the shared ABI and the real mailbox command's failure behavior."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class WifiTransportTests(unittest.TestCase):
    def compile_and_run(self, source):
        with tempfile.TemporaryDirectory() as temporary:
            c_path = Path(temporary) / "transport.c"
            binary = Path(temporary) / "transport"
            c_path.write_text(source)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                            "-Werror", "-I", str(ROOT / "shared"), str(c_path),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_heap_snapshot_and_released_ring_space(self):
        firmware = (ROOT / "firmware/esp32s31-linux-companion/components/esp_hosted/shm_slave_api.c").read_text()
        host = (ROOT / "kernel/esp32s31-hosted/esp_shm.c").read_text()
        sample = re.search(r"static void shm_sample_heap\(void\)\n\{.*?\n\}",
                           firmware, re.S).group()
        callback = re.search(r"static void IRAM_ATTR shm_allocation_failed\(.*?\n\}",
                             firmware, re.S).group()
        initialize = re.search(r"static interface_handle_t \*shm_init\(void\)\n\{.*?\n\}",
                               firmware, re.S).group()
        self.assertIn("static uint32_t s_allocation_failures;", firmware)
        show = re.search(r"static ssize_t firmware_heap_show\(.*?\n\}", host, re.S).group()
        done = re.search(r"static void esp_shm_rx_done\(.*?\n\}", host, re.S).group()
        firmware_done = re.search(r"static void shm_rx_done\(.*?\n\}", firmware, re.S).group()
        self.assertIn("static DEVICE_ATTR_RO(firmware_heap);", host)
        self.assertIn("esp_shm_rx_done(ctx, head);", host)
        self.assertIn("shm_rx_done(head);", firmware)
        self.compile_and_run(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "esp32s31-hosted-shm.h"
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
#define IRAM_ATTR
#define ESP_OK 0
#define INIT 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef uint32_t u32;
static uint32_t shared[ESP_SHM_SIZE / 4];
#undef ESP_SHM_ADDR
#define ESP_SHM_ADDR ((uintptr_t)shared)
static unsigned int kicks, firmware_kicks;
static uint32_t s_allocation_failures;
typedef void (*esp_alloc_failed_hook_t)(size_t, uint32_t, const char *);
static esp_alloc_failed_hook_t failed_hook;
static unsigned int registrations, semaphore_creations;
static int registration_error;
typedef struct { int state; } interface_handle_t;
static interface_handle_t s_handle;
static void *s_rx_sem, *s_space_sem;
static int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    assert(callback);
    registrations++;
    if (registration_error) return registration_error;
    failed_hook = callback;
    return ESP_OK;
}
static void *xSemaphoreCreateBinary(void)
{ assert(failed_hook); return (void *)(uintptr_t)++semaphore_creations; }
static void vSemaphoreDelete(void *semaphore) { (void)semaphore; }
#define esp_intr_alloc(...) ESP_OK
struct esp_shm_context { int unused; };
struct device { struct esp_shm_context *context; };
struct device_attribute { int unused; };
static const struct { u32 ctrl; } to_host = {ESP_SHM_TO_HOST_CTRL};
static const struct { u32 ctrl; } to_fw = {ESP_SHM_TO_FW_CTRL};
static void shm_store(u32 offset, u32 value) { shared[offset / 4] = value; }
static u32 shm_load(u32 offset) { return shared[offset / 4]; }
static u32 shm_read32(struct esp_shm_context *context, u32 offset)
{ (void)context; return shared[offset / 4]; }
static void shm_write32(struct esp_shm_context *context, u32 offset, u32 value)
{ (void)context; shared[offset / 4] = value; }
static uint32_t heap_caps_get_free_size(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return caps & MALLOC_CAP_DMA ? 24576 : 32768;
}
static uint32_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return caps & MALLOC_CAP_DMA ? 4096 : 8192;
}
static uint32_t heap_caps_get_largest_free_block(uint32_t caps)
{ assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)); return 12288; }
static struct esp_shm_context *dev_get_drvdata(struct device *device)
{ return device->context; }
static int sysfs_emit(char *buffer, const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = vsnprintf(buffer, 192, format, args);
    va_end(args);
    return result;
}
#define mb() ((void)0)
#define shm_fence() ((void)0)
static void esp_shm_ring_firmware(struct esp_shm_context *context)
{ (void)context; kicks++; }
static void ring_host_doorbell(void) { firmware_kicks++; }
static void shm_flash_init(void (*kick)(void)) { assert(kick == ring_host_doorbell); }
''' + callback + "\n" + sample + "\n" + initialize + "\n" + show + "\n" + done + "\n" + firmware_done + r'''
int main(void)
{
    struct esp_shm_context context;
    struct device device = {&context};
    char output[192];
    _Static_assert(ESP_SHM_FW_ALLOC_FAILURES_OFF == 0x24 &&
                   ESP_SHM_FW_ALLOC_FAILURES_OFF + 4 <= ESP_SHM_TO_HOST_CTRL,
                   "allocation counter stays in unused control words");
    registration_error = 1;
    assert(!shm_init() && !semaphore_creations && !failed_hook);
    registration_error = 0;
    assert(shm_init() == &s_handle && s_handle.state == INIT);
    assert(registrations == 2 && semaphore_creations == 2);
    assert(shared[ESP_SHM_MAGIC_OFF / 4] == ESP_SHM_MAGIC);
    assert(shared[ESP_SHM_FW_ALLOC_FAILURES_OFF / 4] == 0);
    failed_hook(1600, MALLOC_CAP_DMA, "heap_caps_malloc");
    failed_hook(32, MALLOC_CAP_8BIT, "heap_caps_calloc");
    memset(shared, 0xa5, sizeof(shared));
    shm_sample_heap();
    assert(firmware_heap_show(&device, 0, output) > 0);
    assert(!strcmp(output, "free_internal=32768 minimum_internal=8192 free_dma=24576 minimum_dma=4096 largest_dma=12288 allocation_failures=2\n"));
    assert(shared[(ESP_SHM_CTRL + ESP_SHM_CACHE_LOCK_OFF) / 4] == 0xa5a5a5a5);
    assert(shared[(ESP_SHM_TO_HOST_CTRL + ESP_SHM_HEAD_OFF) / 4] == 0xa5a5a5a5);
    shared[(to_host.ctrl + ESP_SHM_WAIT_OFF) / 4] = 1;
    esp_shm_rx_done(&context, 0xfffffff0);
    assert(shared[(to_host.ctrl + ESP_SHM_TAIL_OFF) / 4] == 0xfffffff0);
    assert(!shared[(to_host.ctrl + ESP_SHM_WAIT_OFF) / 4] && kicks == 1);
    esp_shm_rx_done(&context, 0);
    assert(shared[(to_host.ctrl + ESP_SHM_TAIL_OFF) / 4] == 0 && kicks == 1);
    shared[(to_fw.ctrl + ESP_SHM_WAIT_OFF) / 4] = 1;
    shm_rx_done(0xfffffff0);
    assert(shared[(to_fw.ctrl + ESP_SHM_TAIL_OFF) / 4] == 0xfffffff0);
    assert(!shared[(to_fw.ctrl + ESP_SHM_WAIT_OFF) / 4] && firmware_kicks == 1);
    shm_rx_done(0);
    assert(shared[(to_fw.ctrl + ESP_SHM_TAIL_OFF) / 4] == 0 && firmware_kicks == 1);
    return 0;
}
''')

    def test_wire_bounds_and_counter_wrap(self):
        self.compile_and_run(r'''
#include <assert.h>
#include <limits.h>
#include "esp32s31-hosted-shm.h"
_Static_assert(ESP_SHM_VERSION == 2, "firmware ABI");
_Static_assert(ESP_SHM_FLASH_DATA_SIZE == 2048, "mailbox capacity");
_Static_assert(ESP_SHM_FLASH_DATA + ESP_SHM_FLASH_DATA_SIZE <= ESP_SHM_TO_FW_DATA,
               "mailbox must not overwrite transport rings");
_Static_assert(ESP_SHM_TO_HOST_DATA + ESP_SHM_TO_HOST_SIZE == ESP_SHM_SIZE,
               "SHM mapping covers both rings");
int main(void)
{
    const unsigned int size = ESP_SHM_TO_HOST_SIZE;
    unsigned int tail = UINT_MAX - 15U;
    assert(esp_shm_range_valid(0, ESP_SHM_DATA_FLASH_SIZE, ESP_SHM_DATA_FLASH_SIZE));
    assert(esp_shm_range_valid(ESP_SHM_DATA_FLASH_SIZE, 0, ESP_SHM_DATA_FLASH_SIZE));
    assert(!esp_shm_range_valid(ESP_SHM_DATA_FLASH_SIZE, 1, ESP_SHM_DATA_FLASH_SIZE));
    assert(!esp_shm_range_valid(ULLONG_MAX, 2, ESP_SHM_DATA_FLASH_SIZE));
    assert(!esp_shm_range_valid(1, ULLONG_MAX, ESP_SHM_DATA_FLASH_SIZE));
    assert(esp_shm_record_valid(16, tail, 24, 12, size));
    assert(esp_shm_record_valid(size, 0, size - 4, 12, size));
    assert(!esp_shm_record_valid(size + 4, 0, 12, 12, size));
    assert(!esp_shm_record_valid(2, 0, 12, 12, size));
    assert(!esp_shm_record_valid(16, 0, 16, 12, size));
    assert(!esp_shm_record_valid(16, 0, 0, 12, size));
    assert(!esp_shm_record_valid(16, 0, UINT_MAX, 12, size));
    assert(!esp_shm_record_valid(16, 0, size, 12, size));
    assert(esp_shm_flash_request_valid(ESP_SHM_FLASH_PROGRAM, 4095, 1));
    assert(esp_shm_flash_request_valid(ESP_SHM_FLASH_PROGRAM, 2048, 2048));
    assert(!esp_shm_flash_request_valid(ESP_SHM_FLASH_PROGRAM, 4095, 2));
    assert(!esp_shm_flash_request_valid(ESP_SHM_FLASH_PROGRAM, 0, 2049));
    assert(!esp_shm_flash_request_valid(ESP_SHM_FLASH_PROGRAM, 0, 0));
    assert(!esp_shm_flash_request_valid(ESP_SHM_FLASH_PROGRAM, UINT_MAX, 1));
    assert(esp_shm_flash_request_valid(ESP_SHM_FLASH_ERASE,
           ESP_SHM_DATA_FLASH_SIZE - ESP_SHM_FLASH_BLOCK, ESP_SHM_FLASH_BLOCK));
    assert(!esp_shm_flash_request_valid(ESP_SHM_FLASH_ERASE, 1, ESP_SHM_FLASH_BLOCK));
    assert(!esp_shm_flash_request_valid(ESP_SHM_FLASH_ERASE, 0, ESP_SHM_FLASH_SECTOR));
    assert(!esp_shm_flash_request_valid(1, 0, ESP_SHM_FLASH_BLOCK));
    return 0;
}
''')

    def test_timeout_does_not_reuse_pending_mailbox(self):
        driver = (ROOT / "kernel/esp32s31-hosted/esp_shm_flash.c").read_text()
        command = re.search(r"static int esp_shm_flash_cmd\(.*?\n\}", driver, re.S).group()
        callbacks = "\n".join(re.search(r"static int esp_shm_flash_" + name +
                                       r"\(.*?\n\}", driver, re.S).group()
                              for name in ("read", "write", "erase"))
        source = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp32s31-hosted-shm.h"
typedef uint32_t u32;
typedef uint64_t u64;
typedef long long loff_t;
typedef unsigned char u_char;
#define ESP_SHM_FLASH_TIMEOUT_MS 10000
struct mtd_info { uint64_t size; };
struct erase_info { uint64_t addr, len, fail_addr; };
struct esp_shm_flash {
    void *mbox, *data, *doorbell;
    int wq, lock;
    void *window;
    struct mtd_info mtd;
};
#define container_of(pointer, type, member) ((type *)((char *)(pointer) - offsetof(type, member)))
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
#define min_t(type, left, right) ((type)(left) < (type)(right) ? (type)(left) : (type)(right))
static struct esp_shm_flash *current;
static unsigned int writes, copies, wait_mode;
static u32 mbox_read(struct esp_shm_flash *flash, u32 offset)
{ return *(u32 *)((unsigned char *)flash->mbox + offset); }
static void iowrite32(u32 value, void *destination)
{ *(u32 *)destination = value; writes++; }
static void memcpy_toio(void *destination, const void *source, u32 bytes)
{ memcpy(destination, source, bytes); copies++; }
static unsigned int mock_wait(unsigned int timeout)
{
    assert(timeout == 10000);
    if (!wait_mode) return 0;
    *(u32 *)((unsigned char *)current->mbox + ESP_SHM_FLASH_STATUS_OFF) = wait_mode == 2;
    *(u32 *)((unsigned char *)current->mbox + ESP_SHM_FLASH_CMD_OFF) = 0;
    return 1;
}
#define wmb() ((void)0)
#define rmb() ((void)0)
#define msecs_to_jiffies(value) (value)
#define wait_event_timeout(queue, condition, timeout) mock_wait(timeout)
'''
        source += command + "\n" + callbacks
        source += r'''
int main(void)
{
    u32 mailbox[8] = {0}, before[8], doorbell = 0;
    unsigned char data[ESP_SHM_FLASH_DATA_SIZE], source[ESP_SHM_FLASH_DATA_SIZE];
    struct esp_shm_flash flash = {.mbox=mailbox, .data=data, .doorbell=&doorbell,
                                 .window=data, .mtd={ESP_SHM_DATA_FLASH_SIZE}};
    struct erase_info erase = {0, ESP_SHM_FLASH_BLOCK, 0};
    size_t retlen;
    unsigned int count;
    current = &flash;
    memset(source, 0xa5, sizeof(source));
    assert(esp_shm_flash_cmd(&flash, ESP_SHM_FLASH_PROGRAM,
           ESP_SHM_DATA_FLASH_SIZE - 1, source, 2) == -EINVAL);
    assert(esp_shm_flash_cmd(&flash, ESP_SHM_FLASH_PROGRAM, 0, 0, 1) == -EINVAL);
    assert(!writes && !copies);
    assert(esp_shm_flash_cmd(&flash, ESP_SHM_FLASH_PROGRAM, 2048,
           source, sizeof(source)) == -ETIMEDOUT);
    assert(mbox_read(&flash, ESP_SHM_FLASH_CMD_OFF) == ESP_SHM_FLASH_PROGRAM);
    assert(!memcmp(source, data, sizeof(source)));
    assert(writes == 4 && copies == 1 && doorbell == 1);
    memcpy(before, mailbox, sizeof(before));
    assert(esp_shm_flash_cmd(&flash, ESP_SHM_FLASH_ERASE, 0, 0,
           ESP_SHM_FLASH_BLOCK) == -EBUSY);
    assert(!memcmp(before, mailbox, sizeof(before)) && writes == 4 && copies == 1);
    mailbox[ESP_SHM_FLASH_CMD_OFF / 4] = 0; /* Firmware eventually completes. */
    wait_mode = 1;
    assert(esp_shm_flash_cmd(&flash, ESP_SHM_FLASH_ERASE, 0, 0,
           ESP_SHM_FLASH_BLOCK) == 0);
    wait_mode = 2;
    assert(esp_shm_flash_cmd(&flash, ESP_SHM_FLASH_PROGRAM, 0, source, 1) == -EIO);
    count = writes;
    assert(esp_shm_flash_write(&flash.mtd, ESP_SHM_DATA_FLASH_SIZE - 1,
           2, &retlen, source) == -EINVAL && !retlen);
    assert(esp_shm_flash_write(&flash.mtd, -1, 1, &retlen, source) == -EINVAL);
    erase.addr = UINT64_MAX - ESP_SHM_FLASH_BLOCK + 1;
    erase.len = ESP_SHM_FLASH_BLOCK;
    assert(esp_shm_flash_erase(&flash.mtd, &erase) == -EINVAL);
    erase.addr = ESP_SHM_DATA_FLASH_SIZE - ESP_SHM_FLASH_BLOCK;
    erase.len = 2 * ESP_SHM_FLASH_BLOCK;
    assert(esp_shm_flash_erase(&flash.mtd, &erase) == -EINVAL);
    assert(writes == count); /* Reject the entire request before modifying valid data. */
    assert(esp_shm_flash_read(&flash.mtd, -1, 1, &retlen, source) == -EINVAL);
    assert(esp_shm_flash_read(&flash.mtd, ESP_SHM_DATA_FLASH_SIZE - 1,
           2, &retlen, source) == -EINVAL);
    mailbox[ESP_SHM_FLASH_CMD_OFF / 4] = ESP_SHM_FLASH_PROGRAM;
    memset(source, 0, sizeof(source));
    assert(esp_shm_flash_read(&flash.mtd, 0, 1, &retlen, source) == -EBUSY && !retlen);
    assert(source[0] == 0); /* Timed-out writes must not expose a partial read. */
    mailbox[ESP_SHM_FLASH_CMD_OFF / 4] = 0;
    wait_mode = 1;
    assert(esp_shm_flash_read(&flash.mtd, 0, 1, &retlen, source) == 0);
    assert(retlen == 1 && source[0] == data[0]);
    assert(esp_shm_flash_write(&flash.mtd, 2047, 2, &retlen, source) == 0);
    assert(retlen == 2 && writes == count + 8); /* Split across the 2 KiB mailbox. */
    erase.addr = 0;
    erase.len = 2 * ESP_SHM_FLASH_BLOCK;
    assert(esp_shm_flash_erase(&flash.mtd, &erase) == 0 && writes == count + 16);
    return 0;
}
'''
        self.compile_and_run(source)
