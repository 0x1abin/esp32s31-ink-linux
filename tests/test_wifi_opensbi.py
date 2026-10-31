"""Exercise the actual OpenSBI PMA and cache-service integration."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class WifiOpenSBITests(unittest.TestCase):
    def test_cache_vendor_service_registers_and_syncs_cpu1_instructions(self):
        platform = (ROOT / "platform/esp32s31/opensbi/platform.c").read_text()
        vendor = (ROOT / "upstream/opensbi/lib/sbi/sbi_ecall_vendor.c").read_text()
        framework = (ROOT / "upstream/opensbi/include/sbi/sbi_platform.h").read_text()

        def function(source, declaration):
            return re.search(declaration + r"\(.*?\n\{.*?\n\}", source, re.S).group()

        constants = platform[platform.index("#define ESP32S31_CACHE_BASE"):
                             platform.index("#define ESP32S31_CACHE_LOCK")]
        constants += "\n".join(re.search(r"^#define " + name + r"\s+[^\n]+", platform, re.M).group()
                                for name in ("ESP32S31_PSRAM_BASE", "ESP32S31_PSRAM_SIZE",
                                             "ESP32S31_FW_BASE", "ESP32S31_FW_SIZE"))
        ops = re.search(r"const struct sbi_platform_operations platform_ops = \{.*?\n\};",
                        platform, re.S).group()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef unsigned long ulong;
#define SBI_ENOTSUPP -2
#define SBI_EINVAL -3
#define SBI_EXT_VENDOR_START 0x09000000UL
#define SBI_EXT_VENDOR_END 0x09ffffffUL
#define CSR_MVENDORID 0xf11
#define csr_read(csr) (0x612UL)
struct sbi_trap_regs { ulong a0, a1; };
struct sbi_ecall_return { ulong value; };
struct sbi_platform_operations {
    void *early_init, *final_init, *timer_init;
    bool (*vendor_ext_check)(void);
    int (*vendor_ext_provider)(long, struct sbi_trap_regs *, struct sbi_ecall_return *);
};
struct sbi_platform { const struct sbi_platform_operations *ops; };
#define sbi_platform_ops(plat) ((plat)->ops)
#define esp32s31_early_init NULL
#define esp32s31_final_init NULL
#define esp32s31_timer_init NULL
struct sync_operation { ulong map, op, address, size; };
static struct sync_operation operations[4];
static unsigned int sync_count;
static ulong current_hartid(void) { return 1; }
static void esp32s31_cache_sync(ulong map, ulong op, ulong address, ulong size)
{
    assert(sync_count < 4);
    operations[sync_count++] = (struct sync_operation){map, op, address, size};
}
''' + constants + "\n" + "\n".join((
            function(platform, r"static bool cache_range_valid"),
            function(platform, r"static bool esp32s31_vendor_ext_check"),
            function(platform, r"static int esp32s31_vendor_ext"), ops)) + r'''
static struct sbi_platform platform = {.ops = &platform_ops};
static const struct sbi_platform *sbi_platform_thishart_ptr(void) { return &platform; }
''' + "\n".join((
            function(framework, r"static inline bool sbi_platform_vendor_ext_check"),
            function(framework, r"static inline int sbi_platform_vendor_ext_provider"))) + r'''
struct sbi_ecall_extension {
    ulong extid_start, extid_end;
    int (*handle)(ulong, ulong, struct sbi_trap_regs *, struct sbi_ecall_return *);
};
static struct sbi_ecall_extension *registered;
static int sbi_ecall_register_extension(struct sbi_ecall_extension *extension)
{ registered = extension; return 0; }
''' + "\n".join((
            function(vendor, r"static inline unsigned long sbi_ecall_vendor_id"),
            function(vendor, r"static int sbi_ecall_vendor_handler"),
            "struct sbi_ecall_extension ecall_vendor;",
            function(vendor, r"static int sbi_ecall_vendor_register_extensions"))) + r'''
int main(void)
{
    struct sbi_ecall_return out = {.value = 123};
    struct sbi_trap_regs regs = {.a0 = 0x50ec3000, .a1 = 4096};
    assert(!sbi_ecall_vendor_register_extensions());
    assert(registered && registered->extid_start == 0x09000612);
    assert(registered->extid_end == registered->extid_start);
    registered->handle = sbi_ecall_vendor_handler;
    assert(!registered->handle(registered->extid_start,
                              ESP32S31_SBI_ICACHE_SYNC_RANGE, &regs, &out));
    assert(!out.value && sync_count == 2);
    assert(operations[0].map == (1UL << 4));
    assert(operations[0].op == ESP32S31_CACHE_WRITEBACK);
    assert(operations[1].map == (1UL << 1));
    assert(operations[1].op == ESP32S31_CACHE_INVALIDATE);
    for (unsigned int i = 0; i < 2; i++) {
        assert(operations[i].address == regs.a0 && operations[i].size == regs.a1);
    }
    sync_count = 0;
    assert(!registered->handle(registered->extid_start,
                              ESP32S31_SBI_ICACHE_SYNC, &regs, &out));
    assert(sync_count == 2 && !operations[0].address && !operations[0].size);
    assert(!operations[1].address && !operations[1].size);
    sync_count = 0; regs.a0 = 0x2f020000;
    assert(registered->handle(registered->extid_start,
                             ESP32S31_SBI_ICACHE_SYNC_RANGE, &regs, &out) == SBI_EINVAL);
    assert(!sync_count);
    assert(registered->handle(registered->extid_start, 99, &regs, &out) == SBI_ENOTSUPP);
    assert(!sync_count);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "cache-vendor.c"
            executable = Path(temporary) / "cache-vendor"
            fixture.write_text(code)
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                           ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            str(fixture), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_pma_initializer_emits_custom_csr_writes(self):
        prefixes = []
        if os.environ.get("CROSS_COMPILE"):
            prefixes.append(os.environ["CROSS_COMPILE"])
        prefixes += [str(ROOT / f"outputs/{profile}/host/bin/riscv64-esp-linux-musl-")
                     for profile in ("buildroot-eink4-plus-wifi", "buildroot-eink4-plus")]
        prefix = next((item for item in prefixes
                       if shutil.which(item + "gcc") and shutil.which(item + "objdump")), None)
        if prefix is None:
            self.skipTest("build a Metalio target to install its RISC-V cross-toolchain")
        platform = (ROOT / "platform/esp32s31/opensbi/platform.c").read_text()
        constants = platform[platform.index("#define ESP32S31_PMAADDR0"):
                             platform.index("static inline void reg_write")]
        initializer = re.search(r"static void esp32s31_pma_init\(void\)\n\{.*?\n\}",
                                platform, re.S).group()
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "pma.c"
            compiled = Path(temporary) / "pma.o"
            fixture.write_text('#include <sbi/riscv_asm.h>\n' + constants +
                               initializer.replace("static void", "void", 1))
            subprocess.run([prefix + "gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-march=rv32imac_zicsr_zifencei", "-mabi=ilp32", "-ffreestanding",
                            "-I", str(ROOT / "upstream/opensbi/include"), "-c", str(fixture),
                            "-o", str(compiled)], check=True)
            assembly = subprocess.check_output([prefix + "objdump", "-dr", str(compiled)], text=True)
        # csr_write_num() supports a fixed CSR list and panics for these SoC CSRs.
        self.assertRegex(assembly, r"\bcsrw\s+0xbd2,")
        self.assertRegex(assembly, r"\bcsrw\s+0xbc2,")
        self.assertNotIn("csr_write_num", assembly)
        self.assertNotIn("R_RISCV_CALL", assembly)


if __name__ == "__main__":
    unittest.main()
