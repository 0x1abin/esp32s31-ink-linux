// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_system.h"
#include "esp_private/wifi.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "bootloader_random.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp32s31/rom/ets_sys.h"
#include "riscv/csr.h"
#include "hal/assist_debug_ll.h"
#include "hal/cache_ll.h"
#include "hal/cpu_utility_ll.h"
#include "hal/wdt_hal.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "soc/reg_base.h"
#include "soc/spi_mem_c_reg.h"
#include "shm_flash.h"
#include "esp_mmu_map.h"
#include "esp32s31-hosted-shm.h"
#include "boot-image.h"

#define PSRAM_BASE                  0x50000000U
#define OPENSBI_LOAD_ADDR           0x50F00000U
#define OPENSBI_LOAD_SIZE           0x00030000U
#define KERNEL_LOAD_ADDR            0x40400000U
#define DTB_LOAD_ADDR               0x50EC0000U
#define ROOTFS_MAP_ADDR             0x40800000U
#define KERNEL_MAGIC2_OFFSET        0x38U
#define KERNEL_MAGIC2               0x05435352U
#define PSRAM_PMA_SIZE              0x01000000U
#define PSRAM_UNCACHED_BASE         0xC0000000U
#define LINUX_HART                  1U

/* 256 KiB of PSRAM for the heap of the firmware (fw-heap in esp32s31.dtsi). */
#define FW_PSRAM_HEAP_ADDR          0x50E80000U
#define FW_PSRAM_HEAP_SIZE          0x00040000U
_Static_assert(DTB_LOAD_ADDR >= FW_PSRAM_HEAP_ADDR + FW_PSRAM_HEAP_SIZE &&
               DTB_LOAD_ADDR + METALIO_DTB_MAX_SIZE <= OPENSBI_LOAD_ADDR - 0x1000,
               "temporary DTB must not overlap firmware or OpenSBI guard");

/* Owned exclusively by the Linux scan driver. */
#define LINUX_DMA_POOL_ADDR         0x2F030000U
#define LINUX_DMA_POOL_SIZE         0x00040000U

SOC_RESERVE_MEMORY_REGION(LINUX_DMA_POOL_ADDR,
                          LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE,
                          linux_dma_pool);

/* The ROM start code of hart 1 uses this stack: keep it out of the heap. */
#define ROM_HART1_STACK_TOP         0x2F07E600U
#define ROM_HART1_STACK_SIZE        0x00000400U

_Static_assert(ESP_SHM_ADDR + ESP_SHM_SIZE <= LINUX_DMA_POOL_ADDR,
               "shared transport must not overlap display DMA memory");
_Static_assert(LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE <=
               ROM_HART1_STACK_TOP - ROM_HART1_STACK_SIZE,
               "display DMA memory must not overlap the ROM hart 1 stack");

SOC_RESERVE_MEMORY_REGION(ROM_HART1_STACK_TOP - ROM_HART1_STACK_SIZE,
                          ROM_HART1_STACK_TOP,
                          rom_hart1_stack);

/* One nibble (X bit 0, W bit 1, R bit 2) for REE modes 0 to 2 and for TEE. */
#define APM_REGION_ATTR_ALL_RWX     0x7777U

/* R/W for secure and non-secure access, no ECC; same bits on all banks. */
#define MSPI_PMS_ATTR_RW \
    (SPI_FMEM_C_PMS0_RD_ATTR | SPI_FMEM_C_PMS0_WR_ATTR | \
     SPI_FMEM_C_PMS0_NONSECURE_RD_ATTR | SPI_FMEM_C_PMS0_NONSECURE_WR_ATTR)

static const char *TAG = "s31-linux-loader";

struct apm_block {
    uint32_t region0_start;
    uint32_t region0_end;
    uint32_t region0_attr;
    uint32_t region_filter_en;
    uint32_t func_ctrl;
};

static const struct apm_block apm_blocks[] = {
    { CPU_APM_REGION0_ADDR_START_REG, CPU_APM_REGION0_ADDR_END_REG,
      CPU_APM_REGION0_ATTR_REG, CPU_APM_REGION_FILTER_EN_REG,
      CPU_APM_FUNC_CTRL_REG },
    { HP_APM_REGION0_ADDR_START_REG, HP_APM_REGION0_ADDR_END_REG,
      HP_APM_REGION0_ATTR_REG, HP_APM_REGION_FILTER_EN_REG,
      HP_APM_FUNC_CTRL_REG },
    { HP_MEM_APM_REGION0_ADDR_START_REG, HP_MEM_APM_REGION0_ADDR_END_REG,
      HP_MEM_APM_REGION0_ATTR_REG, HP_MEM_APM_REGION_FILTER_EN_REG,
      HP_MEM_APM_FUNC_CTRL_REG },
};

static const uint32_t apm_status_clr_regs[] = {
    HP_APM_M0_STATUS_CLR_REG, HP_APM_M1_STATUS_CLR_REG,
    HP_APM_M2_STATUS_CLR_REG, HP_APM_M3_STATUS_CLR_REG,
    HP_APM_M4_STATUS_CLR_REG, HP_APM_M5_STATUS_CLR_REG,
    HP_APM_M6_STATUS_CLR_REG,
    HP_MEM_APM_M0_STATUS_CLR_REG, HP_MEM_APM_M1_STATUS_CLR_REG,
    HP_MEM_APM_M2_STATUS_CLR_REG, HP_MEM_APM_M3_STATUS_CLR_REG,
    HP_MEM_APM_M4_STATUS_CLR_REG, HP_MEM_APM_M5_STATUS_CLR_REG,
    CPU_APM_M0_STATUS_CLR_REG, CPU_APM_M1_STATUS_CLR_REG,
    CPU_APM_M2_STATUS_CLR_REG, CPU_APM_M3_STATUS_CLR_REG,
};

static void fence_i(void)
{
    __asm__ volatile ("fence.i" ::: "memory");
}

static void flush_l1_cache_before_handoff(void)
{
    /* A preempting task must not dirty PSRAM between writeback and invalidate. */
    unsigned int irq = portSET_INTERRUPT_MASK_FROM_ISR();

    cache_ll_writeback_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_DATA, CACHE_LL_ID_ALL);
    cache_ll_invalidate_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL);
    fence_i();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
}

/* Open each APM: one region, all addresses, RWX for all modes and masters. */
static void open_apm(void)
{
    for (size_t i = 0; i < sizeof(apm_blocks) / sizeof(apm_blocks[0]); i++) {
        const struct apm_block *b = &apm_blocks[i];

        REG_WRITE(b->region0_start, 0);
        REG_WRITE(b->region0_end, 0xFFFFFFFF);
        REG_WRITE(b->region0_attr, APM_REGION_ATTR_ALL_RWX);
        REG_WRITE(b->region_filter_en, 1);
        REG_WRITE(b->func_ctrl, 0xFFFFFFFF);
    }

    /* The four PMS sections of the flash and of the PSRAM controller. */
    for (int i = 0; i < 4; i++) {
        const uint32_t psram_off = DR_REG_PSRAM_MSPI0_BASE - DR_REG_FLASH_SPI0_BASE;

        REG_WRITE(SPI_FMEM_C_PMS0_ATTR_REG + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_SMEM_C_PMS0_ATTR_REG + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_FMEM_C_PMS0_ATTR_REG + psram_off + i * 4, MSPI_PMS_ATTR_RW);
        REG_WRITE(SPI_SMEM_C_PMS0_ATTR_REG + psram_off + i * 4, MSPI_PMS_ATTR_RW);
    }

    /* Clear the permission faults that the filters latched while they opened. */
    for (size_t i = 0;
         i < sizeof(apm_status_clr_regs) / sizeof(apm_status_clr_regs[0]); i++) {
        REG_WRITE(apm_status_clr_regs[i], 1);
    }
}

static void disable_watchdog(wdt_inst_t inst)
{
    wdt_hal_context_t ctx;

    wdt_hal_init(&ctx, inst, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_disable(&ctx);
}

static void disable_watchdogs(void)
{
    disable_watchdog(WDT_MWDT0);
    disable_watchdog(WDT_MWDT1);
    disable_watchdog(WDT_RWDT);
}

/* The stack-spill monitor of hart 1 would trigger on kernel stacks. */
static void disable_linux_hart_stack_protector(void)
{
    assist_debug_ll_sp_spill_monitor_disable(LINUX_HART);
    assist_debug_ll_sp_spill_interrupt_disable(LINUX_HART);
    assist_debug_ll_sp_spill_set_min(LINUX_HART, 0);
    assist_debug_ll_sp_spill_set_max(LINUX_HART, 0xffffffff);
}

/* The heap of the firmware in PSRAM, with the capabilities that esp_psram gives. */
static void add_psram_heap(void)
{
    uint32_t caps[] = { MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT, 0,
                        MALLOC_CAP_8BIT | MALLOC_CAP_32BIT };
    esp_err_t err = heap_caps_add_region_with_caps(caps, FW_PSRAM_HEAP_ADDR,
                                                   FW_PSRAM_HEAP_ADDR + FW_PSRAM_HEAP_SIZE);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no PSRAM heap: %s", esp_err_to_name(err));
    }
}

/* Lock hart 0 out of the memory of hart 1 (docs/internals.md, "Protection"). */
#define PMP_NO_ACCESS               (PMP_L | PMP_NAPOT)
#define PMP_NO_ACCESS_TOR           (PMP_L | PMP_TOR)
#define PMP_READ_WRITE              (PMP_L | PMP_NAPOT | PMP_R | PMP_W)

/* The first matching PMP entry wins, so allow CPU0 buffers before Linux deny. */
static bool protect_linux_memory(void)
{
    const uint32_t heap = PMPADDR_NAPOT(FW_PSRAM_HEAP_ADDR,
                                      FW_PSRAM_HEAP_ADDR + FW_PSRAM_HEAP_SIZE);
    const uint32_t cached = PMPADDR_NAPOT(PSRAM_BASE, PSRAM_BASE + PSRAM_PMA_SIZE);
    const uint32_t alias = PMPADDR_NAPOT(PSRAM_UNCACHED_BASE,
                                       PSRAM_UNCACHED_BASE + PSRAM_PMA_SIZE);
    PMP_RESET_AND_ENTRY_SET(10, heap, PMP_READ_WRITE);
    PMP_RESET_AND_ENTRY_SET(11, cached, PMP_NO_ACCESS);
    PMP_RESET_AND_ENTRY_SET(12, alias, PMP_NO_ACCESS);
    PMP_RESET_AND_ENTRY_SET(13, 0, 0);
    /* The original 256 KiB pool is not NAPOT-aligned; TOR preserves both edges. */
    PMP_RESET_AND_ENTRY_SET(14, LINUX_DMA_POOL_ADDR, 0);
    PMP_RESET_AND_ENTRY_SET(15, LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE,
                            PMP_NO_ACCESS_TOR);

    /* IDF returns the CSR shifted back by two; NAPOT's low two bits are lost. */
    if (PMP_ENTRY_CFG_READ(10) != PMP_READ_WRITE ||
        PMP_ENTRY_ADDR_READ(10) != (heap & ~3U) ||
        PMP_ENTRY_CFG_READ(11) != PMP_NO_ACCESS ||
        PMP_ENTRY_ADDR_READ(11) != (cached & ~3U) ||
        PMP_ENTRY_CFG_READ(12) != PMP_NO_ACCESS ||
        PMP_ENTRY_ADDR_READ(12) != (alias & ~3U) ||
        PMP_ENTRY_CFG_READ(13) != 0 || PMP_ENTRY_ADDR_READ(13) != 0 ||
        PMP_ENTRY_CFG_READ(14) != 0 ||
        PMP_ENTRY_ADDR_READ(14) != LINUX_DMA_POOL_ADDR ||
        PMP_ENTRY_CFG_READ(15) != PMP_NO_ACCESS_TOR ||
        PMP_ENTRY_ADDR_READ(15) != LINUX_DMA_POOL_ADDR + LINUX_DMA_POOL_SIZE
        ) {
        ESP_LOGE(TAG, "PMP entries 10-15 are in use: Linux memory not protected");
        return false;
    }
    return true;
}

/* Reset entry of hart 1.  esp_cpu_configure_region_protection() locks PMP. */
static void IRAM_ATTR linux_hart_entry(void)
{
    /* The ROM PMA entry gives hart 1 no write to PSRAM; a lower index wins. */
    PMA_ENTRY_SET_NAPOT(2, PSRAM_BASE, PSRAM_PMA_SIZE,
                        PMA_NAPOT | PMA_EN | PMA_R | PMA_W | PMA_X);

    /* OpenSBI forwards hart ID in a0 and the temporary DTB in a1. */
    __asm__ volatile (
        "li   a0, %0\n\t"
        "li   a1, %2\n\t"
        "li   t0, %1\n\t"
        "jr   t0"
        :: "i"(LINUX_HART), "i"(OPENSBI_LOAD_ADDR), "i"(DTB_LOAD_ADDR)
        : "a0", "a1", "t0");
}

static void start_linux_hart(void)
{
    esp_cpu_unstall(LINUX_HART);
    cpu_utility_ll_enable_clock_and_reset_app_cpu();
    ets_set_appcpu_boot_addr((uint32_t)linux_hart_entry);
}

static const esp_partition_t *find_partition(const char *name)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, name);
    if (!part) {
        ESP_LOGE(TAG, "%s partition not found", name);
    }
    return part;
}

/* Copy the start of a partition to PSRAM. */
static bool read_partition(const esp_partition_t *part, uint32_t addr,
                           size_t size)
{
    esp_err_t err = esp_partition_read(part, 0, (void *)addr, size);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading %s partition failed: %s", part->label,
                 esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static bool map_linux_partition(const char *name, uint32_t address,
                                uint32_t flash_address, uint32_t size,
                                mmu_mem_caps_t caps)
{
    const esp_partition_t *part = find_partition(name);
    void *mapped = NULL;

    if (!part || part->address != flash_address || part->size != size)
        return false;
    esp_err_t err = esp_mmu_map_virt(address, part->address, size,
                                   MMU_TARGET_FLASH0, caps, 0, &mapped);
    if (err != ESP_OK || mapped != (void *)address) {
        ESP_LOGE(TAG, "%s XIP mapping failed: %s", name, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool load_dtb_partition(void)
{
    const esp_partition_t *part = find_partition("dtb");
    uint8_t header[8];

    if (!part || !read_partition(part, (uint32_t)header, sizeof(header)))
        return false;
    uint32_t size = metalio_dtb_size(header, part->size);
    if (!size) {
        ESP_LOGE(TAG, "invalid device tree header");
        return false;
    }
    return read_partition(part, DTB_LOAD_ADDR, size);
}

static bool prepare_linux(void)
{
    if (!map_linux_partition("linux", KERNEL_LOAD_ADDR, 0x500000, 0x400000,
                             MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT) ||
        !map_linux_partition("rootfs", ROOTFS_MAP_ADDR, 0x900000, 0x600000,
                             MMU_MEM_CAP_READ | MMU_MEM_CAP_8BIT) ||
        !load_dtb_partition())
        return false;
    if (*(const volatile uint32_t *)(KERNEL_LOAD_ADDR + KERNEL_MAGIC2_OFFSET) !=
        KERNEL_MAGIC2) {
        ESP_LOGE(TAG, "invalid RISC-V XIP image header");
        return false;
    }
    return true;
}

static bool load_opensbi_partition(void)
{
    const esp_partition_t *part = find_partition("opensbi");

    if (!part) {
        return false;
    }

    if (part->size < OPENSBI_LOAD_SIZE) {
        ESP_LOGE(TAG, "opensbi partition size 0x%08" PRIx32
                      " is smaller than the loaded window 0x%08" PRIx32,
                 part->size, (uint32_t)OPENSBI_LOAD_SIZE);
        return false;
    }

    /* fw_jump has no size manifest: a fixed window, OpenSBI clears its BSS. */
    if (!read_partition(part, OPENSBI_LOAD_ADDR, OPENSBI_LOAD_SIZE)) {
        return false;
    }

    ESP_LOGI(TAG, "opensbi copied: flash=0x%08" PRIx32 " size=0x%08" PRIx32
                  " psram=0x%08" PRIx32,
             part->address, (uint32_t)OPENSBI_LOAD_SIZE,
             (uint32_t)OPENSBI_LOAD_ADDR);
    return true;
}

/* A restart at full speed loops and scrolls the error away: wait first. */
static void restart_after_error(void)
{
    ESP_LOGE(TAG, "restart in 5 s");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}

static int discard_log(const char *format, va_list args)
{
    (void)format;
    (void)args;
    return 0;
}

void esp_hosted_main(void);

void app_main(void)
{
    open_apm();
    disable_watchdogs();
    /* ESP-IDF stops the TRNG after the 2nd stage; the Linux hwrng needs it. */
    bootloader_random_enable();

    if (!esp_psram_is_initialized() || esp_psram_get_size() < PSRAM_PMA_SIZE) {
        ESP_LOGE(TAG, "PSRAM not initialized");
        restart_after_error();
        return;
    }

    if (!load_opensbi_partition() || !prepare_linux()) {
        restart_after_error();
        return;
    }

    add_psram_heap();

    ESP_LOGI(TAG, "handoff: OpenSBI=0x%08" PRIx32 " kernel=0x%08" PRIx32,
             (uint32_t)OPENSBI_LOAD_ADDR, (uint32_t)KERNEL_LOAD_ADDR);

    /* A radio start accesses the flash (cache off): do it before hart 1. */
    esp_hosted_main();
    if (*(volatile uint32_t *)(ESP_SHM_ADDR + ESP_SHM_MAGIC_OFF) != ESP_SHM_MAGIC ||
        *(volatile uint32_t *)(ESP_SHM_ADDR + ESP_SHM_VERSION_OFF) != ESP_SHM_VERSION ||
        *(volatile uint32_t *)(ESP_SHM_ADDR + ESP_SHM_FLASH + ESP_SHM_FLASH_SIZE_OFF) !=
        ESP_SHM_DATA_FLASH_SIZE) {
        ESP_LOGE(TAG, "radio transport or persistent Flash window not ready");
        restart_after_error();
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_internal_set_log_level(WIFI_LOG_NONE));
    disable_linux_hart_stack_protector();
    flush_l1_cache_before_handoff();

    shm_flash_share_cache();
    if (!protect_linux_memory()) {
        restart_after_error();
        return;
    }
    /* Linux on CPU1 owns the display, touch and serial consoles after handoff. */
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_set_vprintf(discard_log);
    start_linux_hart();
}
