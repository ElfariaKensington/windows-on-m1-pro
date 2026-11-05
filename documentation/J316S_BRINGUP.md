# J316s M1 Pro bring-up

The target is the 2021 16-inch MacBook Pro, `MacBookPro18,1`, board `J316s`,
SoC `T6000`, with two Icestorm and eight Firestorm cores. The changes here
prepare the hypervisor for that CPU topology. The J316s firmware and native
Windows Setup milestone are still open.

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

## Remaining work before native Setup

- **Firmware and memory.** Add a J316s guest layout and a matching Mu target.
  Check guest RAM, firmware and boot arguments, the low-memory alias, and the
  framebuffer against the live boot memory map. Aurora's J316s target defaults
  to native AIC firmware with a different Windows startup path, while this
  project runs Mu behind m1n1's virtual GIC. Use Aurora's board evidence to
  inform the port and verify the compiled ACPI against the virtual GIC.
- **Installer media and display.** For the first milestone, use a RAM-backed
  ARM64 Windows PE installer and a basic framebuffer. Review the DCP handoff
  on this board and keep the GPU driver out of the first build. Reuse the
  existing preloaded ramdisk format after giving it a validated J316s
  reservation. [Microsoft Windows PE](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-intro)
- **Storage and DMA.** The J313 autonomous path maps broad Apple MMIO ranges
  and backs its guest NVMe controller with the physical SSD. A J316s
  installer-only profile needs an explicit MMIO allowlist that excludes
  internal storage and its DMA control paths, and must omit the synthetic
  NVMe controller. Before enabling physical USB, derive its port resources
  from the J316s ADT and verify the AIC2 and DART routing separately.

The first hardware checkpoint would be the Windows Setup UI with the SSD
absent from the guest. It requires a separate test Mac or a verified DFU
recovery path. The J313 ESP installer and launch paths reject this board.
No native boot was attempted on the target during this work.
