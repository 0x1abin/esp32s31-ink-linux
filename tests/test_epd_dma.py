"""Exercise the actual coherent SRAM allocation, including its failure branch."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class EpdDmaTests(unittest.TestCase):
    def test_sram_allocation_bounds_and_failure(self):
        driver = (ROOT / 'kernel/metalio-epd.c').read_text()
        allocation = driver[driver.index('\t/* One allocation fits the pool'):
                            driver.index('\tepd->pending = kvzalloc')]
        code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/metalio_epd.h>
#define EPD_DESC_OFFSET 209856U
#define EPD_DESC_BYTES 848U
#define EPD_DMA_BYTES (EPD_DESC_OFFSET + EPD_DESC_BYTES)
#define GFP_KERNEL 0
struct device { int unused; };
struct epd_descriptor { uint32_t words[4]; };
struct metalio_epd {
    unsigned char *phase;
    uintptr_t phase_dma, descriptors_dma;
    struct epd_descriptor *descriptors;
};
static int fail, allocations;
static void *dmam_alloc_coherent(struct device *dev, size_t size, uintptr_t *dma, int flags) {
    assert(dev && size == EPD_DMA_BYTES && flags == GFP_KERNEL);
    assert(size > (1U << 17) && size <= (1U << 18));
    allocations++;
    if (fail) return NULL;
    *dma = 0x2f030000;
    void *data = malloc(size); assert(data); memset(data, 0xa5, size); return data;
}
static int allocate(struct metalio_epd *epd, struct device *dev) {
    int ret;
''' + allocation + r'''
    return 0;
fail: return ret;
}
int main(void) {
    _Static_assert(METALIO_EPD_ABI_VERSION == 1, "display ABI");
    _Static_assert(sizeof(struct metalio_epd_info) == 32, "info ABI");
    _Static_assert(sizeof(struct metalio_epd_refresh) == 16, "request ABI");
    _Static_assert(sizeof(struct metalio_epd_status) == 16, "status ABI");
    struct device dev = {0};
    struct metalio_epd epd = {0};
    assert(!allocate(&epd, &dev));
    assert(allocations == 1 && epd.phase_dma == 0x2f030000);
    assert(epd.descriptors_dma == epd.phase_dma + EPD_DESC_OFFSET);
    assert(epd.descriptors_dma + EPD_DESC_BYTES <= 0x2f070000);
    assert((void *)epd.descriptors == epd.phase + EPD_DESC_OFFSET);
    for (size_t i = 0; i < EPD_DMA_BYTES; i++) assert(epd.phase[i] == 0);
    free(epd.phase);
    memset(&epd, 0, sizeof(epd)); fail = 1;
    assert(allocate(&epd, &dev) == -ENOMEM);
    assert(allocations == 2 && !epd.phase && !epd.descriptors);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'dma.c'
            binary = Path(temporary) / 'dma'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', '-I', str(ROOT / 'include/uapi'), str(source),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
