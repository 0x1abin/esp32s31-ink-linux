"""Buildroot's actual configuration must produce scans with no extra IEs."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class WifiScanTests(unittest.TestCase):
    def test_profile_removes_default_scan_ies_rejected_by_hosted(self):
        source = next((ROOT / f"outputs/{profile}/build/wpa_supplicant-2.11"
                       for profile in ("buildroot-eink4-plus-wifi",)
                       if (ROOT / f"outputs/{profile}/build/wpa_supplicant-2.11/wpa_supplicant/Makefile").is_file()), None)
        if source is None:
            self.skipTest("build a Wi-Fi target to install the pinned wpa_supplicant source")
        wpa = (source / "wpa_supplicant/wpa_supplicant.c").read_text()
        functions = "\n".join(re.search(declaration + r"\(.*?\n\{.*?\n\}", wpa, re.S).group()
                              for declaration in ("static void wpas_ext_capab_byte",
                                                  "int wpas_build_ext_capab"))
        makefile = (source / "wpa_supplicant/Makefile").read_text()
        implicit_wnm = "\n".join(re.findall(r"ifdef CONFIG_[A-Z_]+\nCONFIG_WNM=y\nendif", makefile))
        self.assertIn("CONFIG_BGSCAN_SIMPLE", implicit_wnm)
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
struct wpa_bss { int unused; };
struct wpa_config { int coloc_intf_reporting, disable_btm, ftm_responder, ftm_initiator; };
struct wpa_supplicant {
    struct wpa_config *conf;
    int disable_mbo_oce;
    unsigned int extended_capa_len;
    u8 *extended_capa, *extended_capa_mask;
};
#define WLAN_EID_EXT_CAPAB 127
#define WLAN_EXT_CAPAB_SCS 54
#define WLAN_EXT_CAPAB_MSCS 85
#define wpa_printf(...) ((void)0)
static bool wpas_driver_bss_selection(struct wpa_supplicant *wpa_s)
{ (void)wpa_s; return false; }
static bool wpa_bss_ext_capab(struct wpa_bss *bss, int capability)
{ (void)bss; (void)capability; return false; }
''' + functions + r'''
int main(void)
{
    struct wpa_config conf = {0};
    struct wpa_supplicant supplicant = {.conf = &conf};
    u8 ies[18] = {0};
    int len = wpas_build_ext_capab(&supplicant, ies, sizeof(ies), NULL);
#ifdef EXPECT_NO_SCAN_IES
    assert(len == 0);
#else
    /* Captured on hardware before nl80211 rejected the trigger with EINVAL. */
    const u8 original[] = {0x7f, 0x0b, 0, 0, 0x0a, 2, 0, 0, 0x40, 0, 0, 0, 0x20};
    assert(len == sizeof(original) && !memcmp(ies, original, sizeof(original)));
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fixture = directory / "scan-ies.c"
            fixture.write_text(code)
            for profile in (False, True):
                with self.subTest(wifi_profile=profile):
                    build = directory / ("wifi" if profile else "defaults")
                    (build / "wpa_supplicant").mkdir(parents=True)
                    shutil.copy2(source / "wpa_supplicant/defconfig",
                                 build / "wpa_supplicant/defconfig")
                    config = build / "wpa_supplicant/.config"
                    configure = directory / "configure.mk"
                    configure.write_text(f"""BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH := {ROOT}
WPA_SUPPLICANT_DIR := {build}
BR2_PACKAGE_WPA_SUPPLICANT_NL80211 := y
include {ROOT}/upstream/buildroot/package/wpa_supplicant/wpa_supplicant.mk
""" + (f"include {ROOT}/platform/esp32s31/companion.mk\n" if profile else "") + f"""
{build}/configured:
\t$(WPA_SUPPLICANT_CONFIGURE_CMDS)
""")
                    subprocess.run(["make", "--no-print-directory", "-s", "-f", str(configure),
                                    str(build / "configured")], check=True, capture_output=True)
                    # Use the real Makefile's implicit WNM selections too; merely
                    # commenting CONFIG_WNM leaves BGSCAN_SIMPLE re-enabling it.
                    flags = directory / "flags.mk"
                    flags.write_text(f"include {config}\n" + implicit_wnm + """
all:
\t@printf '%s\\n' '$(CONFIG_WNM) $(CONFIG_NO_ROBUST_AV)'
""")
                    wnm, robust = subprocess.check_output(
                        ["make", "--no-print-directory", "-s", "-f", str(flags)],
                        text=True).strip("\n").split(" ")
                    definitions = (["-DCONFIG_WNM"] if wnm else []) + \
                                  (["-DCONFIG_NO_ROBUST_AV"] if robust else []) + \
                                  (["-DEXPECT_NO_SCAN_IES"] if profile else [])
                    executable = build / "scan-ies"
                    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                                   ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                    "-Wno-unused-function", "-Wno-unused-parameter",
                                    "-Wno-sign-compare"] + definitions +
                                   [str(fixture), "-o", str(executable)], check=True)
                    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
