# Licenses and source attribution

This repository's default license is **GPL-2.0-only**, provided in the root
[LICENSE](../LICENSE). Existing file-level SPDX identifiers, copyright notices
and third-party licenses take precedence for those files. This default does not
relicense upstream projects or reference material.

| Component | License and attribution |
| --- | --- |
| Project build scripts, configuration and documentation, unless marked otherwise | GPL-2.0-only |
| Native Linux display and touch drivers | GPL-2.0-only; touch behavior sources are recorded in [Metalio-touch.md](Metalio-touch.md) |
| Binary phase encoder, waveform endpoints and display pattern demo | MIT; retain [Metalio-reference-MIT.txt](Metalio-reference-MIT.txt) and the provenance in [Metalio-waveform.md](Metalio-waveform.md) |
| Terminal, UI and extracted 8×16 ASCII font | GPL-2.0-only; the font's exact Linux source commit and hashes are recorded in [terminal-font.h](../apps/terminal-font.h) |
| Display UAPI header | GPL-2.0 WITH Linux-syscall-note; the unchanged [Linux-syscall-note](Linux-syscall-note) exception is copied from the pinned Linux source's `LICENSES/exceptions/Linux-syscall-note` |
| Metalio device tree | GPL-2.0 OR MIT, as marked in the source |
| Optional Wi-Fi loader, shared-memory driver, firmware and reference patches | File-level BSD-2-Clause, GPL-2.0-only, Apache-2.0 and the shared header's dual license; see [WiFi.md](WiFi.md) |
| Retained muxOS reference notice | Apache-2.0; [MuxOS-E0470-Apache-2.0.txt](MuxOS-E0470-Apache-2.0.txt) identifies inspected reference material, not code incorporated into the Linux driver |

The MIT and GPL texts under `apps/LICENSES/` are also retained because the
Buildroot application package uses them as its license files. The original
Metalio notice includes the upstream authors' copyright statements.

The firmware also contains separately licensed upstream software. The exact
source revisions are pinned in [sources.lock.json](../sources.lock.json), and
package versions and license files are recorded in Buildroot package recipes.
Important components include:

| Upstream component | License source |
| --- | --- |
| Linux | GPL-2.0-only with syscall exception and individual file licenses; upstream `COPYING` and `LICENSES/` |
| U-Boot | GPL version 2 with individual file licenses; upstream `Licenses/README` and `Licenses/` |
| OpenSBI | BSD-2-Clause and individual file licenses; upstream `COPYING.BSD` |
| Buildroot | GPL-2.0-or-later with individual exceptions; upstream `COPYING` |
| libvterm | MIT; its `LICENSE`, declared in `package/libvterm/` |
| Dropbear client and bundled crypto libraries | MIT, BSD-2-Clause, Public domain, Unlicense and WTFPL; declared in `package/dropbear-client-only/` |
| BusyBox and other selected packages | Their upstream license files, collected by Buildroot's `legal-info` target |
| Prebuilt toolchain and its musl/GCC runtime libraries | The toolchain's `share/licenses/` directory; the release packager includes these notices separately because the custom external-toolchain recipe does not collect them automatically |

Before distributing a firmware binary, run the documented Buildroot
`legal-info` target and retain its package license files, manifests and available
source archives alongside the corresponding project source. The project license
notices alone do not replace the notices and source required for the complete
firmware. Review the generated `legal-info/README` for packages that Buildroot
cannot collect automatically, including locally supplied kernel and bootloader
trees and the custom external toolchain. The release packager also includes the
available upstream Linux, U-Boot, OpenSBI, Buildroot and BSP license notices;
this does not collect their complete corresponding source trees.
