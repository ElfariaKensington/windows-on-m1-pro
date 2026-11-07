# J316 SPI input

Independent Windows ARM64 implementation for the J316s built-in keyboard and
trackpad. No previous AppleInput implementation or historical commit is used.
The MIT license at the repository root applies to this code. GPL source was read
to establish hardware behavior; no Linux or m1n1 code is incorporated here.

## Source and ownership contract

Inspected on 2026-11-07:

* Live MacBookPro18,1 IODeviceTree: SPI3 `0x39b10c000`, AP GPIO
  `0x39b028000` / enable 194, NUB GPIO `0x2921f0000` / active-low input 6.
* Live IOHIDDevice keyboard descriptor: report 1, modifiers, reserved byte,
  six key usages and one byte containing consumer/Fn state (10 input bytes).
* Asahi Linux `asahi`: `drivers/spi/spi-apple.c`,
  `drivers/hid/spi-hid/spi-hid-apple-{core,of}.c`,
  `drivers/hid/hid-magicmouse.c`, `drivers/pinctrl/pinctrl-apple-gpio.c`,
  `arch/arm64/boot/dts/apple/t600x-j314-j316.dtsi`.
* Asahi m1n1 `main`: `proxyclient/m1n1/hw/spi.py` and
  `proxyclient/experiments/spi.py` (register semantics, not board settings).
* Current project integration: `hv_j316s_setup.c` resource filtering, pinmux and
  clock ownership, and J316sSetupPkg DSDT.
* [Microsoft VHF source driver guide](https://learn.microsoft.com/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)
  and the official 10.0.28000.2526 ARM64 WDK headers/libraries.

EL2 owns SPI3 clock gating, SPI pinmux, filtered MMIO and physical interrupt
routing. Mu owns the ACPI resource declaration. This driver matches all three
resource addresses and lengths before mapping them; it owns enable/reset,
bounded PIO, protocol discovery, runtime reports and power-down. No DMA is used.
Only the enable pin and input pin are touched in their GPIO controllers.

Unlike Linux's threaded interrupt handler, a passive one-shot WDF timer samples
the active-low input every 4 ms. It does not enable the guest interrupt. This
avoids depending on an unqualified Windows interrupt path, while preserving
Linux's SPI mode 0, 8-bit words, 8 MHz maximum, 65 us CS setup/hold, 250 us CS
inactive delay, 200 us write/status turnaround and high/low/high reset sequence.
The source clock is the board's 24 MHz controller input. Controller transfers
have absolute deadlines; each polling turn drains a bounded number of packets.

Keyboard reports use the descriptor discovered from the device and Microsoft's
inbox keyboard stack. LED output is queued to the passive transport worker.
Trackpad data is decoded into an inbox HID mouse: one-finger motion, physical
click/drag, two-finger right-click and vertical/horizontal scrolling. Contact
count changes reset motion history to avoid cursor jumps. This is not a Windows
Precision Touchpad implementation; tap-to-click and advanced gestures are not
advertised. A touch-controller reset schedules multitouch re-enablement.

`protocol.c` is freestanding and host-testable. It rejects bad packet/message
CRCs, inconsistent fragment totals, gaps, oversized input and malformed touch
frames. Windows lifecycle and MMIO are in `driver.c`.

## Checkpoint and recovery

The first native checkpoint is driver StartDevice, device boot notification,
successful descriptor discovery, keyboard typing and trackpad movement in Setup.
Offline build/tests do not prove these. Preserve the currently installed ESP
image and manifest before staging. The installation image changes only the
boot WIM's input package; it retains the current boot/firmware contract. Return
to macOS through Startup Options to restore the ESP backup if input fails.

## Build and verification

Restore the official NuGet packages pinned in `packages.config` into
`.build/j316s-fresh-input/<lowercase-package-id>/`, then run:

```sh
bash drivers/j316-spi-input/build.sh
python3 -m unittest tests.test_j316_spi_input -q
```

The macOS cross-build uses clang-cl and LLD with the real WDK headers,
KMDF 1.33 entry/loader libraries and VHF library. `JI_PACKAGES` and `JI_OUTPUT`
override the package and output directories. `JI_CERTIFICATE=/path/cert.cer`
also builds the native certificate-import/diagnostic utility.

For a standard Windows WDK signing machine, `sign.ps1` signs the driver,
runs Inf2Cat, signs the catalog and verifies the result. The local macOS
development package uses an embedded SYS signature and a catalog generated
by Microsoft's MakeCat inside a disposable ARM64 WinPE guest. Windows PnP
accepted that package. A DevCon-created test node with no hardware resources
reached the driver's resource validation and returned `0xC0000182`, as expected;
this tests kernel loading, KMDF AddDevice, VHF filter installation and refusal
to map absent hardware. It does not test the physical SPI bus.

`build-image.py --help` documents the signed-package, pristine WIM, prepared BCD
and official wimboot inputs. It verifies WIM integrity and Mu's compiled
resources, then emits a payload and SHA-256 manifest. `stage.py` uses that
manifest's full ESP images, rechecks the exact board/partition identity and
current disk hash, writes only the ESP and verifies every byte. `--restore`
performs the reverse operation. Neither command reboots the machine.

Native logs are in `X:\J316Input`. When the existing diagnostics volume is
found through `J316S-DIAGNOSTICS.TARGET`, they are copied to `J316FreshInput`.
Phase 10 means discovery and VHF source startup completed. The keyboard/mouse
counters count decoded hardware reports, not delivery to Windows applications.

## First native result and failure diagnostics

The first native boot reached Setup but neither built-in input worked. The
diagnostics disk had no `J316FreshInput` directory, so no SPI startup boundary
can be inferred from that run. A package-load success message was insufficient.

Revision 2 saves startup phase, NTSTATUS, packet counters and SPI/GPIO state in
the driver's Parameters registry key before returning a failed start. The
diagnostic utility reads this snapshot when the failed device cannot be opened.
The initial screen is held for 20 seconds and explicitly distinguishes package
installation from hardware startup. Phase 2 waits for the boot notification;
3 queries identity; 4–8 query interface information/descriptors; 9 creates HID;
10 runs input; 11 rejects the resource set. SPI transaction behavior is unchanged
in this diagnostic revision. It is not a claimed native-input fix.
Setup also enables guest-volume automount and rescans before collecting logs;
logs go only to the marked `J316S_DIAG` internal diagnostics volume.
The base Setup WIM has no `timeout.exe`; waits use Kernel32 Sleep in the
diagnostic utility, so the screen delay and logger do not depend on it.

The next native photo showed phase 1/status `0x103` (pending) and zero packets.
That was a single early sample, not proof that reset stalled: DrvLoad completes
while PnP startup is still asynchronous. The screen now refreshes seven times
over 21 seconds. Checkpoints 12–16 distinguish completed SPI setup, completed
input-pin setup and each high/low/high reset delay. SPI transactions are
unchanged; the next causal decision must use the final refreshed sample.

## Discovery correction from the native recording

The final recording sample reached phase 4 with `0xc00000b5`, identity
`05ac:0343`, 19 valid packets, 14 keyboard reports and 2 mouse reports.
Reset, SPI receive and input decoding were operating. The Windows source
aborted at the management interface-information query before creating VHF HID
devices, so received input could not reach Windows.

Discovery now treats interface-information responses as advisory, following
[Asahi's probe and response dispatcher](https://github.com/AsahiLinux/linux/blob/asahi/drivers/hid/spi-hid/spi-hid-apple-core.c).
The source no longer requires the response counter to echo the request counter;
one outstanding query is matched by response type, report and interface after
packet/message CRC validation. Identity and HID-descriptor discovery remain
required. The recording did not capture the response counter, so its exact
contribution to the timeout is unproven.

Both regressions failed with the previous strict policy and pass with this
change. Tests cover missing optional metadata, required-query failures,
different response counters, wrong interfaces/types, interleaved input and bad
CRCs. The correction still requires native keyboard/touchpad validation.

## WinPE HID child binding

The next native recording reached phase 10/status 0 with a 90-byte keyboard
descriptor, 128 keyboard reports, 449 mouse reports and no submission errors.
Input was still unusable. A resource-free ARM64 Windows probe reproduced the
downstream fault: SetupAPI could not include `MsHidKmdf.inf` and rejected both
VHF children with `0xe0000219` (no associated function driver). This Setup WIM
contains Microsoft's `mshidkmdf.sys` but omits its Include/Needs-only INF.

The source now supplies a private `J316SPI\VHF_INPUT` hardware ID, and
`J316VhfInput.inf` binds those children to the existing Microsoft miniport.
It contains the complete service installation and has no missing INF dependency.
No Microsoft binary is replaced or redistributed in this source tree.
The main INF stages the bridge with CopyINF; the Setup script also stages it
before loading the source.

VHF retains `HID_DEVICE_SYSTEM_VHF` as its first hardware ID, which makes the
broken inbox INF outrank our second, specific ID. `J316Tools.exe /bind` therefore
waits for exactly two present `J316SPI` children and uses Microsoft's
UpdateDriverForPlugAndPlayDevices API to select their bridge. Startup retries
only until the first successful binding. No other VHF source matches the ID.
Both instance-ID lengths include their terminating NUL; previously VHF truncated
the final character.

`vhf_probe.c` and `input_observer.c` provide a Windows integration regression.
The probe shares `vhf_binding.h` and the production mouse descriptor, uses the
live keyboard's measured descriptor, and generates alternating A-key and relative
mouse reports. Its separate test INF must match only `ROOT\J316VHFP`.
It must never be installed into the native boot image.
The VM has virtual NVMe storage and a framebuffer, with no input devices attached.
`vhf-regression.cmd` runs the actual production startup script and helper;
the observer must report nonzero key-down samples AND cursor changes, and PnP
must show started `kbdhid/kbdclass` and `mouhid/mouclass` stacks.
This validates Windows HID delivery; physical M1 Pro operation still requires
the user's native test.
The final production-startup regression recorded binding result 0, 759 key-down
samples, 101 cursor changes, and both started class stacks. Without the bridge,
the same descriptor/report probe recorded zero keys and zero cursor changes.

References: [Microsoft VHF architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-),
[VHF configuration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/ns-vhf-_vhf_config),
[forced selection of a matching device driver](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-updatedriverforplugandplaydevicesw).
