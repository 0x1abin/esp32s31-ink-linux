# Metalio FT6336U touch behavior reference

The project-owned Linux driver and its protocol/state helpers are independently
written under GPL-2.0-only. They implement the register protocol and the
Metalio behavior; no Apache-licensed source is incorporated into the kernel.

The following files were inspected at muxOS-project commit
`c6a3e5f436e5b44234be4a5e2c3985abad480e8e`:

| Reference file | SHA-256 |
| --- | --- |
| `firmware/boards/packages/mux_touch_metalio/ft6336u.c` | `abd71ab0caf7d9ac59e5b0b87faa51a711ef4c51de5ffb35b32b3cd148c1768d` |
| `firmware/boards/packages/mux_touch_metalio/mux_touch_metalio.c` | `b44324490b2337b83320ea3a27166a5873f3664fee0e7e1277998ecbbb099268` |
| `firmware/boards/packages/mux_power_metalio/mux_power_metalio.c` | `97fa0c9e2b3c444e4648c6fe6bcb2abfc69845bfbf2a4b8c431b20f89f7966da` |
| `firmware/tests/metalio/test_touch_coordinates.c` | `1fd4ddc61604fa7a76b177b84cb1bcd926bd7e389591e92f173d921251a9a5ba` |

The reference touch source is marked Apache-2.0. Its coordinate helper additionally
identifies the original Metalio `b400605` MIT implementation. These notices record
behavior provenance, not a claim that the reference code has been relicensed.
The retained Metalio MIT notice is `Metalio-reference-MIT.txt`.

The Linux implementation reads thirteen bytes beginning at register 0x02,
validates up to two contacts and their four-bit IDs, and treats DOWN/CONTACT as
active and UP as released. It preserves first-contact screen/strip/ignored
classification until release. A malformed report or I2C failure releases all
reported input and suppresses new presses until a valid report has no active
raw contacts.

Portrait normalization accepts coordinates below 1216 in both axes except when
both are at least 684. Coordinates with raw X below 684 are used directly;
otherwise the output is `(683 - raw Y, raw X)`. The Linux input device directly
reports portrait coordinates; the reference framework's subsequent native-axis
conversion and 90-degree rotation are not performed.

Strip zones are raw Y 1570 through 1630 and raw X within 30 of 100, 200 or 300.
The leftmost zone at X 100 reports KEY_REFRESH for manual full GC. The other
two page zones at X 200/300 retain one aggregated KEY_ENTER state rather than
the reference UP/DOWN actions. Any held page contact keeps Enter down; a new
Enter press requires both page contacts to be released. Confirm and Enter are
independent states and do not repeat automatically. Screen and strip contacts
remain independent inputs.

The reset pulse is low for at least 10 ms followed by a 300 ms release wait.
GPIO5 uses both edges, supplemented by a nominal 10 ms polling interval with
a minimum 5 ms read spacing, rounded to kernel timer ticks. The shared Linux
I2C bus remains at 100 kHz: the reference sets 400 kHz for touch individually
but uses 100 kHz for the TPS65185 on the same bus. The reference's shared-owner
cache, RTOS queues and framework input rotation are not imported.
