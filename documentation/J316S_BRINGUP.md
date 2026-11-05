# J316s M1 Pro bring-up

The target is the 2021 16-inch MacBook Pro, `MacBookPro18,1`, board `J316s`,
SoC `T6000`, with two Icestorm and eight Firestorm cores. The changes here
prepare the hypervisor for that CPU topology and build a standalone Setup
candidate. Native entry and the Setup screen still need hardware validation.

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
a private framebuffer that m1n1 copies to the existing panel surface.

EL2 compares the stage-2 mapping log against an exact allowlist before
guest entry. It permits the reserved RAM and its low alias, software
hooks for the virtual GIC and emulated serial port, and no physical MMIO.
The J313 disk bridge and writable host ADT alias are outside this path.
This profile aims for the first visible Setup screen. It includes the
ARM64 Setup WIM and boot files on a RAM disk. The install image and
physical keyboard, trackpad and GPU paths are outside that milestone.
[Microsoft Windows PE](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-intro)

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
  --iso-sha256 <microsoft-iso-sha256>
python3 tools/pack_j316s_setup.py \
  --mu-build mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB \
  --loader-elf m1n1_windows/build/m1n1-raw.elf \
  --media setup-arm64.img --output J316s-Setup-unqualified.bin
```

## Hardware qualification

The target Mac has the stock internal partition layout. A native test
would require provisioning a separate Apple-authorized boot environment
before the loader can run. The image is also larger than the ESP in
Asahi's standard example layout. The stage-2 restrictions apply after
m1n1 starts and cannot protect the process of setting up that boot
environment. With no second Mac available for DFU recovery, the
candidate was kept as an ordinary file. The first native checkpoint
remains a visible Setup screen with the SSD absent from Windows.
[Asahi boot layout](https://asahilinux.org/docs/platform/open-os-interop/) ·
[Apple DFU recovery requirements](https://support.apple.com/en-us/108900)
