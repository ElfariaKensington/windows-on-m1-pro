# Windows 11 on the M1 Pro MacBook Pro (J316s)

Native Windows 11 ARM64 on the 2021 16-inch MacBook Pro (`MacBookPro18,1`, board `J316s`,
SoC `T6000`, 2 Icestorm + 8 Firestorm cores). No virtual machine: Windows runs from the
internal SSD through Project Mu firmware and a small m1n1-based hypervisor.

> **Experimental.** This project reaches the internal SSD through experimental firmware and a
> custom NVMe bridge. A wrong `diskpart` command can destroy macOS, Recovery, or the Asahi boot
> environment. Keep a complete backup and read the whole guide before starting.
> Only the M1 Pro MacBook Pro (`J316s`) is supported.

## Status (validated vs. pending)

| Area | State |
|---|---|
| Interrupt controller | Hypervisor virtual GIC corrected for the 10-core T6000 topology (offline checks passed) |
| Setup firmware | Project Mu `J316sSetupPkg` and m1n1 `J316S_SETUP=1` build; wimboot reaches the **Windows 11 Setup GUI** |
| Install | **Not yet verified.** Interactive Setup and installation to the internal SSD have not been completed |
| USB | **Not operational on hardware**: no USB port works in the tested image. The USB profile is opt-in (`--usb`), so an image built without it has no USB. Even with it, native enumeration is unverified |
| Built-in keyboard and trackpad | **Working** (verified on hardware) |
| Ethernet | Not verified |
| Wi-Fi | **Partial**: networks are listed, but connecting fails. The driver is incomplete |
| Internal SSD | Bounded NVMe slice profile built and checked with synthetic GPT data; native ANS run pending |
| GPU | Not started on this machine |

Details and the exact checkpoints for each area are in
[documentation/J316S_BRINGUP.md](documentation/J316S_BRINGUP.md).

## Installation (step by step)

This is a development procedure. Each step below is a checkpoint; stop and read the linked
document if a step does not match what you see.

1. **Confirm the machine.** `MacBookPro18,1` (M1 Pro 16-inch, 2021), board `J316s`. Other models
   are not supported. Check with `ioreg -l | grep -i board-id` or *About This Mac*.
2. **Back up everything.** Save `diskutil list` and `diskutil apfs list` output before changing
   anything.
3. **Provision the Apple-authorized boot environment.** Use the official Asahi installer
   (`curl https://alx.sh | sh`) and choose the UEFI-only profile, as described in
   [documentation/INSTALL.md](documentation/INSTALL.md), section 1. Finish the boot policy in
   paired 1TR recovery and check that stock Asahi UEFI boots before continuing.
4. **Clone with submodules:**
   ```sh
   git clone --recurse-submodules https://github.com/ElfariaKensington/windows-on-m1-pro.git
   cd windows-on-m1-pro
   git submodule status --recursive
   ```
5. **Build the Mu firmware** with `stuart_build` for `Platform/MacBookAirMid2020Pkg/PlatformBuild.py`
   and `--j316s-setup`, using the environment in [documentation/BUILD.md](documentation/BUILD.md).
6. **Build the m1n1 payload** with the J316s profile:
   ```sh
   make -C m1n1_windows J316S_SETUP=1 USE_CLANG=1 ARCH=aarch64-none-elf \
     TOOLCHAIN=/usr/lib/llvm-18/bin/ LLDDIR=/usr/lib/llvm-18/bin/ build/m1n1.bin
   ```
   The candidate was compiled with Clang 18. No compiler/firmware combination is qualified yet.
7. **Prepare the Setup media** from the Microsoft Windows 11 ARM64 ISO, checking its SHA-256:
   ```sh
   python3 tools/build_j316s_setup_media.py windows-arm64.iso setup-arm64.img \
     --wimboot wimboot.arm64 --iso-sha256 <microsoft-iso-sha256>
   ```
   For full installation media on an existing FAT32 USB volume, use `--usb-source` instead
   (see the bring-up document).
8. **Pack the firmware image:**
   ```sh
   python3 tools/pack_j316s_setup.py \
     --mu-build mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB \
     --loader-elf m1n1_windows/build/m1n1-raw.elf \
     --media setup-arm64.img --output J316s-Setup-unqualified.bin
   ```
   Add `--usb` (and build Mu with `BLD_*_J316S_SETUP_USB=TRUE`) if you need USB input or media.
   Without it the image has no USB, and USB is currently not working on hardware.
9. **Start Setup** from the packed image through the Asahi boot entry you prepared in step 3.
   The installer's RAM disk holds the Setup WIM; WinPE reads the full installation source from
   USB when you use the USB profile.
10. **Stop and record the result.** Write down which step you reached, the image hash, and any
    stop code. Installation to the internal SSD is not yet verified.

## Reporting issues

Open an issue in this repository with:

- Mac model (`MacBookPro18,1`), macOS version, and the output of `diskutil list`;
- the packed image name and its SHA-256;
- the step where it failed and the exact message or stop code;
- the Mu and m1n1 commits you built from;
- what you already tried.

Do not attach your full partition layout if you do not want to share it.

## Repository layout

- `m1n1_windows/`: hypervisor fork (submodule).
- `mu/`: Project Mu Apple Silicon fork (submodule).
- `drivers/`: Windows drivers, including the Wi-Fi driver under `drivers/j316-wificx/`.
- `scripts/`: build, packing, and install entry points.
- `tools/`: media builders, packers, and analysis tools.
- `config/`: guest memory layout and board configuration.
- `documentation/`: bring-up, build, and run documentation.
- `investigation/`: experiment ledger. Every hardware run is recorded here.

## Documentation

- [J316s bring-up and checkpoints](documentation/J316S_BRINGUP.md)
- [Build and packaging](documentation/BUILD.md)
- [Installation (Asahi boot environment and Windows media)](documentation/INSTALL.md)
- [Architecture](documentation/ARCHITECTURE.md)
- [Known limitations](documentation/LIMITATIONS.md)
- [Changelog](CHANGELOG.md)

## Upstream projects

This work builds on [Asahi Linux](https://asahilinux.org/),
[m1n1](https://github.com/AsahiLinux/m1n1), [Project Mu](https://github.com/microsoft/mu),
and the Apple Silicon Windows work from [NT-for-ASi](https://github.com/NT-for-ASi).
See the submodule histories and licenses for their respective copyrights and terms.
