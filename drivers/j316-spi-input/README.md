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
Phase 10 means discovery and VHF startup completed; the keyboard/mouse counters
then show whether hardware reports are reaching Windows.
