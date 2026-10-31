import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
LINUX = ROOT / "upstream/linux"


class WifiPhysmapTests(unittest.TestCase):
    def test_full_cramfs_mapping_and_resource_boundaries(self):
        patch = (ROOT / "platform/esp32s31/patches/linux-companion/0002-mtd-physmap-preserve-non-power-of-two-resource-size.patch").read_text()
        with tempfile.TemporaryDirectory() as directory:
            patched = Path(directory) / "physmap-core.c"
            subprocess.run(["patch", "--batch", "--fuzz=0", "-o", str(patched),
                            str(LINUX / "drivers/mtd/maps/physmap-core.c")],
                           input=patch, text=True, check=True, capture_output=True)
            source = patched.read_text()
        sizing = re.search(r"\t\tinfo->win_order = .*?\n\n", source, re.S).group()
        rom = (LINUX / "drivers/mtd/chips/map_rom.c").read_text()
        point = re.search(r"static int maprom_point\(.*?\n\{.*?\n\}", rom, re.S).group()
        binding = "\n".join(re.search(r"\tmtd->" + field + r" = [^;]+;", rom).group()
                            for field in ("priv", "size", "_point"))
        core = (LINUX / "drivers/mtd/mtdcore.c").read_text()
        mtd_point = re.search(r"int mtd_point\(.*?\n\}", core, re.S).group()
        code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
typedef long long loff_t;
typedef uint64_t resource_size_t;
struct map_info { void *virt; resource_size_t phys, size; };
struct gpio_descs { unsigned int ndescs; };
struct physmap_flash_info {
    unsigned int win_order;
    struct map_info maps[1];
    struct gpio_descs *gpios;
};
struct mtd_info {
    void *priv;
    uint64_t size;
    int (*_point)(struct mtd_info *, loff_t, size_t, size_t *, void **, resource_size_t *);
};
#define resource_size(res) (*(res))
#define BIT(order) (1UL << (order))
static unsigned int fls64(uint64_t value) { return 64 - __builtin_clzll(value); }
static struct mtd_info *mtd_get_master(struct mtd_info *mtd) { return mtd; }
static loff_t mtd_get_master_ofs(struct mtd_info *mtd, loff_t from)
{ (void)mtd; return from; }
static uint64_t map_size(uint64_t bytes, struct gpio_descs *gpios)
{
    struct physmap_flash_info storage = {.gpios = gpios}, *info = &storage;
    uint64_t *res = &bytes;
    unsigned int i = 0;
''' + sizing + r'''
    return info->maps[i].size;
}
''' + point + "\n" + mtd_point + r'''
static void bind_rom(struct mtd_info *mtd, struct map_info *map)
{
''' + binding + r'''
}
int main(void)
{
    const size_t mib = 1024 * 1024, image = 4636 * 1024;
    struct map_info map = {.virt = (void *)0x40800000, .phys = 0x40800000};
    struct mtd_info mtd;
    size_t retlen;
    void *virt;
    resource_size_t phys;
    map.size = map_size(6 * mib, NULL);
    bind_rom(&mtd, &map);
    assert(!mtd_point(&mtd, 0, 4096, &retlen, &virt, &phys));
    assert(retlen == 4096 && virt == map.virt && phys == map.phys);
    /* The real boot failed here after its first superblock page succeeded. */
    assert(!mtd_point(&mtd, 0, image, &retlen, &virt, &phys));
    assert(retlen == image && virt == map.virt && phys == map.phys);
    assert(mtd.size == 6 * mib);
    assert(!mtd_point(&mtd, 0, 6 * mib, &retlen, &virt, &phys));
    assert(retlen == 6 * mib);
    assert(!mtd_point(&mtd, 5 * mib, mib, &retlen, &virt, &phys));
    assert(retlen == mib && (uintptr_t)virt == map.phys + 5 * mib);
    assert(mtd_point(&mtd, 0, 6 * mib + 1, &retlen, &virt, &phys) == -EINVAL);
    assert(!retlen && !virt && !phys);
    assert(mtd_point(&mtd, 6 * mib - 1, 2, &retlen, &virt, &phys) == -EINVAL);
    assert(mtd_point(&mtd, 6 * mib, 1, &retlen, &virt, &phys) == -EINVAL);
    assert(mtd_point(&mtd, -1, 1, &retlen, &virt, &phys) == -EINVAL);
    /* Existing power-of-two and GPIO-banked windows keep their sizes. */
    for (unsigned int order = 12; order <= 24; order++) {
        assert(map_size(1UL << order, NULL) == (1UL << order));
        for (unsigned int pins = 0; pins <= 3; pins++) {
            struct gpio_descs gpios = {.ndescs = pins};
            assert(map_size(1UL << order, &gpios) == (1UL << (order + pins)));
        }
    }
    assert(map_size(6 * mib - 1, NULL) == 6 * mib - 1);
    assert(map_size(6 * mib + 1, NULL) == 6 * mib + 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "physmap.c"
            executable = Path(directory) / "physmap"
            fixture.write_text(code)
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                           ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                            str(fixture), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
