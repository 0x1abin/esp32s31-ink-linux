import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ("arch/riscv/kernel/entry.S", "drivers/irqchip/irq-riscv-intc.c",
           "drivers/irqchip/irq-esp-clic.c", "drivers/mtd/maps/physmap-core.c")


class WifiIRQTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.kernel = Path(cls.temporary.name)
        for relative in SOURCES:
            destination = cls.kernel / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / "upstream/linux" / relative, destination)
        subprocess.run([str(ROOT / "platform/esp32s31/patch-companion-kernel.sh"), str(cls.kernel)],
                       check=True, capture_output=True, text=True)

    def test_pending_doorbell_waits_for_child_mapping_and_enable(self):
        source = (self.kernel / "drivers/irqchip/irq-esp-clic.c").read_text()
        constants = source[source.index("#define CLIC_CTRL_OFF"):
                           source.index("struct esp_clic {")]
        helpers = "\n".join(re.search(r"static (?:inline )?(?:int|unsigned int|void) " + name +
                                      r"\(.*?\n\}", source, re.S).group()
                            for name in ("slot_to_idx", "esp_clic_mask", "esp_clic_unmask",
                                         "esp_clic_install_chained"))
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
''' + constants + r'''
struct irq_desc { int unused; };
struct irq_data { unsigned int hwirq; };
struct esp_clic { void *base, *parent_domain; unsigned int intc_virqs[CLIC_EXT_COUNT]; };
static struct esp_clic *clic;
static unsigned char registers[0x2000];
static bool irq_enabled, pending, mapping_ready, mapping_failure;
static unsigned int selected_slot, early_delivery, delivered, chain_calls, creates;
static void *posted_address;
static uint8_t posted_value;
static void inject_pending(void)
{
    if (irq_enabled && pending && registers[CLIC_INTIE_OFF(selected_slot)]) {
        if (!mapping_ready) early_delivery++;
        else { delivered++; pending = false; }
    }
}
#define local_irq_save(flags) do { (flags) = irq_enabled; irq_enabled = false; } while (0)
#define local_irq_restore(flags) do { irq_enabled = (flags); inject_pending(); } while (0)
#define pr_err(...) ((void)0)
static void writeb(uint8_t value, void *address)
{
    assert(!irq_enabled);
    assert(!posted_address);
    posted_address = address; posted_value = value;
}
static void mb(void)
{
    assert(posted_address);
    *(uint8_t *)posted_address = posted_value;
    posted_address = NULL;
}
static void writeb_relaxed(uint8_t value, void *address)
{ *(uint8_t *)address = value; inject_pending(); }
static unsigned int irq_create_mapping(void *domain, unsigned int slot)
{ (void)domain; creates++; return mapping_failure ? 0 : slot + 64; }
static void esp_clic_chained_handler(struct irq_desc *desc) { (void)desc; }
static void irq_set_chained_handler_and_data(unsigned int virq,
                                             void (*handler)(struct irq_desc *), void *data)
{
    assert(virq == selected_slot + 64 && handler == esp_clic_chained_handler && data == clic);
    chain_calls++;
    /* The real IRQ core activates the parent before child-domain insertion. */
    registers[CLIC_INTIE_OFF(selected_slot)] = 1;
    inject_pending();
}
''' + helpers + r'''
int main(void)
{
    for (int entry_enabled = 1; entry_enabled >= 0; entry_enabled--) {
        for (unsigned int slot = CLIC_EXT_FIRST; slot <= CLIC_EXT_LAST; slot++) {
            struct esp_clic controller = {.base = registers};
            struct irq_data child = {.hwirq = slot};
            clic = &controller; selected_slot = slot;
            memset(registers, 0xa5, sizeof(registers));
            registers[CLIC_INTIE_OFF(slot)] = 0;
            irq_enabled = entry_enabled; pending = true; mapping_ready = false;
            mapping_failure = false; early_delivery = delivered = chain_calls = creates = 0;
            assert(!esp_clic_install_chained(clic, slot));
            assert(irq_enabled == !!entry_enabled && !early_delivery && !delivered);
            assert(pending && !registers[CLIC_INTIE_OFF(slot)] && !posted_address);
            assert(chain_calls == 1 && creates == 1);
            for (unsigned int i = 0; i < sizeof(registers); i++)
                if (i != CLIC_INTIE_OFF(slot)) assert(registers[i] == 0xa5);
            /* IRQF_NO_AUTOEN leaves the pending source masked after insertion. */
            mapping_ready = true;
            inject_pending();
            assert(pending && !delivered);
            irq_enabled = true;
            esp_clic_unmask(&child);
            assert(!pending && delivered == 1 && !early_delivery);
            assert(!esp_clic_install_chained(clic, slot));
            assert(chain_calls == 1 && creates == 1 && registers[CLIC_INTIE_OFF(slot)] == 1);
            esp_clic_mask(&child);
            assert(!registers[CLIC_INTIE_OFF(slot)]);
        }
    }
    struct esp_clic failed = {.base = registers};
    clic = &failed; mapping_failure = true; irq_enabled = true; chain_calls = 0;
    assert(esp_clic_install_chained(clic, CLIC_EXT_FIRST) == -ENXIO);
    assert(irq_enabled && !chain_calls && !failed.intc_virqs[0]);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "clic.c"
            executable = Path(directory) / "clic"
            fixture.write_text(code)
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                           ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            str(fixture), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_early_sret_preserves_frame_and_runs_irq_at_base_level(self):
        source = (self.kernel / "arch/riscv/kernel/entry.S").read_text()
        code = source.split("#ifdef CONFIG_ESP32S31_HOSTED\n", 1)[1].split("#endif", 1)[0]
        instructions = [line.strip() for line in code.splitlines()
                        if line.strip() and not line.strip().startswith("/*")]
        self.assertIn("\tcall do_irq", source.split(".Lmetalio_clic_level_popped:", 1)[1])
        # Model the emitted instructions and CLIC sret semantics, rather than a
        # duplicate C implementation of the level adjustment.
        constants = {"SR_SPIE": 0x20, "SR_SPP": 0x100}
        for irq in (5, 16, 31):
            for previous_level in (0, 1, 127, 255):
                for status in (0, 0x20, 0x100, 0x120):
                    raw_cause = 0x80000000 | (previous_level << 16) | irq
                    frame = {"cause": raw_cause, "status": status, "epc": 0x12345678}
                    saved = dict(frame)
                    registers = {"a0": 0x50010000}
                    csrs = {"CSR_STATUS": status, "CSR_CAUSE": raw_cause, "CSR_EPC": frame["epc"]}
                    for line in instructions:
                        if line.endswith(":"):
                            continue
                        mnemonic, *operands = re.split(r"[\s,]+", line)
                        if mnemonic == "REG_L":
                            self.assertEqual(operands[1], "PT_CLIC_SCAUSE(sp)")
                            registers[operands[0]] = frame["cause"]
                        elif mnemonic == "li":
                            registers[operands[0]] = constants.get(operands[1], None)
                            if registers[operands[0]] is None:
                                registers[operands[0]] = int(operands[1], 0)
                        elif mnemonic == "not":
                            registers[operands[0]] = ~registers[operands[1]] & 0xffffffff
                        elif mnemonic == "and":
                            registers[operands[0]] = registers[operands[1]] & registers[operands[2]]
                        elif mnemonic == "csrw":
                            csrs[operands[0]] = registers[operands[1]]
                            if operands[0] == "CSR_CAUSE":
                                # scause aliases SPP/SPIE; force the worst input
                                # state to check that the following writes fix it.
                                csrs["CSR_STATUS"] = 0x20
                        elif mnemonic == "csrc":
                            csrs[operands[0]] &= ~registers[operands[1]]
                        elif mnemonic == "csrs":
                            csrs[operands[0]] |= registers[operands[1]]
                        elif mnemonic == "la":
                            self.assertEqual(operands[1], ".Lmetalio_clic_level_popped")
                            registers[operands[0]] = 0x40401000
                        elif mnemonic == "sret":
                            self.assertTrue(csrs["CSR_CAUSE"] & 0x80000000)
                            self.assertEqual((csrs["CSR_CAUSE"] >> 16) & 0xff, 0)
                            self.assertTrue(csrs["CSR_STATUS"] & constants["SR_SPP"])
                            self.assertFalse(csrs["CSR_STATUS"] & constants["SR_SPIE"])
                            self.assertEqual(csrs["CSR_EPC"], 0x40401000)
                        else:
                            self.fail("unmodeled early trap instruction: " + line)
                    self.assertEqual(frame, saved)
                    self.assertEqual(registers["a0"], 0x50010000)

    def test_clic_timer_enable_uses_mmio_and_preserves_other_bytes(self):
        source = (self.kernel / "drivers/irqchip/irq-riscv-intc.c").read_text()
        functions = []
        for action in ("mask", "unmask"):
            function = re.search(r"static void riscv_intc_irq_" + action + r"\([^\n]+\)\n\{.*?\n\}", source, re.S)
            self.assertIsNotNone(function)
            functions.append(function.group())
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
struct irq_data { unsigned int hwirq; };
static uint8_t registers[128];
static void *metalio_clic;
static unsigned int csr_value;
#define CONFIG_32BIT 1
#define IS_ENABLED(x) (x)
#define BITS_PER_LONG 32U
#define BIT(x) (1U << (x))
#define CSR_IEH 1
#define CSR_IE 0
#define csr_set(reg, val) (csr_value |= (val))
#define csr_clear(reg, val) (csr_value &= ~(val))
static void writeb(uint8_t value, void *address) { *(uint8_t *)address = value; }
''' + "\n".join(functions) + r'''
int main(void)
{
    struct irq_data timer = { .hwirq = 5 }, external = { .hwirq = 31 };
    memset(registers, 0xa5, sizeof(registers));
    metalio_clic = registers;
    riscv_intc_irq_unmask(&timer);
    assert(registers[21] == 1 && !csr_value);
    riscv_intc_irq_mask(&timer);
    assert(registers[21] == 0);
    riscv_intc_irq_unmask(&external);
    assert(registers[125] == 1);
    for (unsigned int i = 0; i < sizeof(registers); ++i)
        if (i != 21 && i != 125) assert(registers[i] == 0xa5);
    metalio_clic = 0;
    riscv_intc_irq_unmask(&timer);
    assert(csr_value == BIT(5));
    riscv_intc_irq_mask(&timer);
    assert(csr_value == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "irq.c"
            fixture.write_text(code)
            executable = Path(directory) / "irq"
            compiler = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", str(fixture), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_consolidated_series_is_repeatable_and_rejects_drift(self):
        script = ROOT / "platform/esp32s31/patch-companion-kernel.sh"
        with tempfile.TemporaryDirectory() as directory:
            kernel = Path(directory)
            expected = {relative: (self.kernel / relative).read_bytes() for relative in SOURCES}
            for relative, content in expected.items():
                destination = kernel / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(content)
            for _ in range(2):
                subprocess.run([str(script), str(kernel)], check=True, capture_output=True, text=True)
                self.assertEqual({relative: (kernel / relative).read_bytes() for relative in SOURCES}, expected)
            # A partially modified integration must fail before changing any file.
            relative = "drivers/irqchip/irq-esp-clic.c"
            expected[relative] = expected[relative].replace(b"writeb(0, priv->base", b"writeb(2, priv->base")
            (kernel / relative).write_bytes(expected[relative])
            result = subprocess.run([str(script), str(kernel)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Cannot apply or verify hosted kernel patch", result.stderr)
            self.assertEqual({relative: (kernel / relative).read_bytes() for relative in SOURCES}, expected)
