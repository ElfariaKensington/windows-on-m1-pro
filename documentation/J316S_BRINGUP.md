# J316s M1 Pro bring-up

The target is the 2021 16-inch MacBook Pro, `MacBookPro18,1`, board `J316s`,
SoC `T6000`, with two Icestorm and eight Firestorm cores. The changes here
prepare the hypervisor for that CPU topology and build a standalone Setup
candidate. The wimboot build has reached the Windows 11 Setup GUI on this
machine. An interactive Setup session and installation remain unverified.

## Evidence for the interrupt controller change

Read-only macOS queries on the target reported `J316sAP`, `arm-io,t6000`,
`aic,2`, and CPU IDs 0–9 in three clusters of sizes 2, 4, and 4. The Apple
device tree reports the core and cluster coordinates. The Asahi device tree
and Aurora MADT independently give the full MPIDR affinities, including
Aff2 for Firestorm. The 42-bit GIC aperture comes from this fork's existing
hypervisor and Mu definitions.

- [Asahi J316s board description](https://github.com/AsahiLinux/linux/blob/6ba0637d1ed9a2e3639dd3fc9eca6cd0483a6434/arch/arm64/boot/dts/apple/t6000-j316s.dts)
- [Asahi T600x CPU topology](https://github.com/AsahiLinux/linux/blob/6ba0637d1ed9a2e3639dd3fc9eca6cd0483a6434/arch/arm64/boot/dts/apple/t600x-common.dtsi)
- [Aurora J316s firmware target](https://github.com/aurora-silicon/mu/blob/063c6b34da94801d1ee9085594e5ac71ae1793b9/Platform/MacBookPro16Late2021Pkg/MacBookPro16Late2021.dsc)
- [Aurora T6000 MADT](https://github.com/aurora-silicon/mu/blob/063c6b34da94801d1ee9085594e5ac71ae1793b9/Silicon/Apple/T600XFamilyPkg/AcpiTables/MADT_Static.aslc)

The original T6000 switch case in `hv_vgicv3_init` fell through to T6022 and
selected 24 redistributors. Each redistributor was assigned an affinity
from the corresponding m1n1 CPU slot, including slots with no CPU. The
T6000 case now checks the ten started CPU slots and publishes ten frames.
The next Mu build must match those CPU UIDs and MPIDRs in its MADT and use
the 64-bit redistributor base `0x5100000000` and length `0x140000`.
PSCI already derives the affinities from the Apple device tree and owns
secondary CPU startup. m1n1 owns the virtual GIC and physical AIC routes.

## Setup firmware and isolation

The Mu build target is `J316sSetupPkg`. Its ACPI namespace describes the ten
CPUs. The firmware volume uses RAM disk, GIC and framebuffer drivers, with
the physical storage, PCI, USB and DART drivers outside the image. The
packer checks the compiled MADT and the firmware volume's module list
before it writes an image.

The m1n1 `J316S_SETUP=1` build recognizes the separate `ASIJ316S` payload.
It validates board identity, image bounds and checksums before reserving
guest RAM from the live boot memory map. It rejects overlap with the
protected-memory carveouts and the existing framebuffer. The guest uses
a private 1728 × 1117 framebuffer that m1n1 doubles onto the existing
3456 × 2234 panel surface. The larger mode has been built and checked offline;
it still needs a native boot check.

EL2 compares the stage-2 mapping log against an exact allowlist before
guest entry. It permits the reserved RAM and its low alias, software
hooks for the virtual GIC and emulated serial port, and no physical MMIO.
The J313 disk bridge and writable host ADT alias are outside this path.
This profile includes the ARM64 Setup WIM and boot files on a RAM disk.
The RAM disk does not contain `sources/install.wim`, and WinPE has no
physical destination disk. A media-driver prompt in this profile does not
establish that a particular Windows storage driver is missing.
[Microsoft Windows PE](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-intro)

### Input and installation media

The live macOS device tree places the built-in input transport on SPI3 and
its interrupt on NUB GPIO pin 6. The
[Asahi board description](https://github.com/AsahiLinux/linux/blob/6ba0637d1ed9a2e3639dd3fc9eca6cd0483a6434/arch/arm64/boot/dts/apple/t600x-j314-j316.dtsi)
also identifies AP GPIO pin 194 for SPI enable. The J313 input contract uses
different controller addresses, AP pin 195 and NUB pin 13. Do not publish
the J313 ACPI resources on J316s. Both WinPE images contain KMDF and VHF,
but the Apple driver still needs a J316s resource and interrupt contract
and a driver package that WinPE will load.

The optional USB profile exposes the left front USB-C port through the
inbox xHCI driver. EL2 validates the live controller and DART resources,
starts the DWC3 core in USB2 host mode, and exposes only its xHCI register
window. Physical AIC IRQ 1207 maps to guest INTID 857. The two dedicated
DARTs translate USB DMA into the guest's low RAM window. The internal SSD
stays outside the guest. The full install image must come from external
media because it is larger than the EFI partition and reserved RAM disk.

To build this profile, add `'BLD_*_J316S_SETUP_USB=TRUE'` to the Mu command
and `--usb` to the packer command. The native checkpoint is to enumerate a
wired keyboard, mouse and USB installation media on a USB2 hub in the
left front port. USB role negotiation and transfers still need a native
check.

To prepare the complete installation source for an existing FAT32 USB volume:

```sh
python3 tools/build_j316s_setup_media.py windows-arm64.iso Windows-Setup-USB \
  --usb-source --iso-sha256 <microsoft-iso-sha256>
```

This mode needs wimlib and 7-Zip. It extracts the complete ISO and verifies
the ARM64 editions. If needed, it splits `sources/install.wim` into
`install.swm` and subsequent parts in the same directory. Copy the contents
of the result to the root of the USB volume. The m1n1 payload still starts
Setup from RAM, and WinPE reads the full installation source from USB.

To add built-in input, also pass `'BLD_*_J316S_SETUP_INPUT=TRUE'` to Mu and
`--apple-input` to the packer. The firmware publishes the J316s SPI and
GPIO resources. EL2 filters the GPIO register accesses to the two input
pins and their interrupt acknowledge bit. The Windows driver selects
the board clock and pins only after matching all resources. Build
AppleInput with the ARM64 WDK, service the Setup WIM with the resulting
package, and pass the serviced WIM to the media builder with `--boot-wim`.
A separate prepared BCD store can be supplied with `--bcd` for a development
signature policy. The standard driver package starts in transport-only
mode. Keep USB input available while verifying enumeration and enabling
the keyboard and trackpad collections.

### Bounded internal Windows disk

Add `'BLD_*_J316S_SETUP_STORAGE=TRUE'` to Mu and `--windows-slice` to
the packer with USB enabled. The profile publishes a single synthetic NVMe
disk. Its backend accepts one host GPT partition of type
`0509C4B9-AC59-4CCD-93F3-8DECB17F35E3` with at least 64 GiB. Both GPT copies
must agree, pass their CRC checks, and describe nonoverlapping partitions.
All Windows disk LBAs are relative to that partition, including the nested
Windows GPT. The host partition table is outside the exposed range.

Mu uses generic UEFI PCI and NVMe drivers to find Windows Boot Manager on
the nested EFI partition. With a blank Windows disk, the boot manager falls
through to RAM-backed Setup. The storage profile deliberately stops if the
required host partition is absent or invalid. This path has been built and
checked with synthetic GPT data. Native ANS enumeration, installation and
first boot still require validation on the target.

The diagnostics-only profile uses the same virtual NVMe path but accepts a
dedicated host GPT slice of at least 1 GiB. Its guest disk contains a nested
GPT Basic Data partition formatted as FAT32. WinPE writes its SetupAPI, PnP,
driver-service, event and AppleInput transport/VHF snapshots there when it
finds the `J316S-DIAGNOSTICS.TARGET` marker. The write backend bounds every
guest LBA to that one host slice. This profile does not require the installer
USB to work and has not yet been qualified against native ANS hardware.

## Reproduce the offline checks

Use the Project Mu environment described in BUILD.md. Add
`--j316s-setup` to the build command. The other Mu setup and update
commands use the same platform builder.

```sh
stuart_build -c Platform/MacBookAirMid2020Pkg/PlatformBuild.py \
  --j316s-setup TOOL_CHAIN_TAG=CLANGPDB TARGET=DEBUG
```

Build the raw m1n1 ELF with the board profile. This candidate was compiled
with Clang 18 in the ARM64 build container. The J313 release instructions
specify a different compiler, and the J316s candidate has no qualified
compiler or firmware combination yet.

```sh
make -C m1n1_windows J316S_SETUP=1 USE_CLANG=1 ARCH=aarch64-none-elf \
  TOOLCHAIN=/usr/lib/llvm-18/bin/ LLDDIR=/usr/lib/llvm-18/bin/ build/m1n1.bin
```

From the repository root, build the Setup media using Python 3.11 or newer,
7-Zip and mtools. Use the hash Microsoft publishes for the source ISO.
The packer also needs `llvm-nm` and `llvm-objcopy`.

```sh
python3 tools/build_j316s_setup_media.py windows-arm64.iso setup-arm64.img \
  --wimboot wimboot.arm64 \
  --iso-sha256 <microsoft-iso-sha256>
python3 tools/pack_j316s_setup.py \
  --mu-build mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB \
  --loader-elf m1n1_windows/build/m1n1-raw.elf \
  --media setup-arm64.img --output J316s-Setup-unqualified.bin
```

## Hardware qualification

The native checkpoint used a separate Apple-authorized boot environment.
The image is larger than the ESP in Asahi's standard example layout.
Stage-2 restrictions apply after m1n1 starts and cannot protect the process
of provisioning a boot environment. Preserve the build that reached Setup
when preparing an image with new hardware access.
[Asahi boot layout](https://asahilinux.org/docs/platform/open-os-interop/) ·
[Apple DFU recovery requirements](https://support.apple.com/en-us/108900)
