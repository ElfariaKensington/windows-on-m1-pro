# Task for Sonnet 5.5: make the J316s USB-C ports work in Windows

## How the operator wants you to work (read first, non-negotiable)

- **Finish everything in one go.** Do not stop after analysis, after a partial fix, or to ask
  "should I continue?". Investigate, implement, build, test offline, package, install to disk,
  record the ledger entry, then report once. The only acceptable stopping point is an image
  installed on the internal ESP that the operator can boot.
- **Do not ask questions.** Make the best engineering decision and state it in one line in the
  final report. The only thing to hand to the operator is a `sudo` command, run through a
  terminal tab so they only type their password (`mcp__terminal__run_in_terminal`).
- **No log-only or probe-only boots.** Every image you install must be a complete attempt at
  working USB that also logs its own evidence (see "Evidence" below).
- **Adapt existing code, do not rewrite.** Reuse m1n1's existing USB/ATC/TPS code, the J313
  xHCI path and Asahi Linux as references.
- **Do not break what works.** Keyboard/trackpad (SPI), Wi-Fi bridge, the NVMe Windows slice and
  the GPU services (J316S_GPU) must keep working. The rollback image must always be available.
- Be concise in chat. Answer in the operator's language (Turkish if they write Turkish).

## Repository and machine

- Repo: `/Users/elfaria/win11/windows-on-m1` (branch `port/j316s-bringup`). Read `AGENTS.md`,
  `documentation/J316S_BRINGUP.md` (USB section), `investigation/J316S_GPU_PORT.md` (current
  image layout and build tooling) and the tail of `investigation/EXPERIMENTS.md`.
- Host: this Mac **is** the target (MacBookPro18,1, J316s, T6000). There is no second machine and
  no m1n1 proxy; evidence only comes from the boot itself.
- Disk is nearly full: check `df -h /System/Volumes/Data` before building; delete only superseded
  build outputs under `.build/` you created, never user data.

## Current state (facts)

- Boot chain: Asahi stub (macOS 13.5 iBoot) → m1n1 (`J316S_SETUP=1`, hypervisor) → Mu
  `J316sSetupPkg` → Windows 11 ARM64 from the private NVMe slice.
- Installed image: **J316s-GPU-002** (`.build/j316s-gpu/esp-J316s-GPU-002.img`, sha256
  `71b188b5…95da`), built by `scripts/build-j316s-gpu.sh` with `--usb --apple-input
  --windows-slice --wifi` and m1n1 flags `J316S_SETUP=1 J316S_WIFI=1 J316S_GPU=1 J316S_GPU_PROBE=1`.
  Mu built with `BLD_*_J316S_SETUP_{USB,INPUT,STORAGE,WIFI,GPU}=TRUE`.
- USB in this profile: m1n1 `hv_j316s_setup.c` `prepare_usb()` / `prepare_usb_port()` starts each
  of the three DWC3 cores (`/arm-io/usb-drd0..2`) in **USB2 host mode**, exposes only the xHCI
  register window as a stage-2 hook (`xhci_mmio`), routes AIC IRQs 0x4a6/0x4b7/0x4c8 to guest
  INTIDs 0x35a/0x359/0x35b, and points the two per-port DARTs at guest low RAM.
  Constants: `m1n1_windows/src/hv_j316s_setup.h`. ACPI: `mu/Platform/J316sSetupPkg/AcpiTables/DSDT.asl`
  (`XHC1..3`, `PNP0D15`).
- Windows side (collected 2026-11-08, `/Users/elfaria/Downloads/file/data/usb/`): all three
  `ACPI\PNP0D15\0..2` xHCI controllers load `USBXHCI` and report **Started, Code 0**, root hubs
  enumerate, but **no device ever enumerates on any port** (operator: "no USB port works").
- So the controllers are alive; the failure is below xHCI: Type-C PHY/port power/role.

## Most likely causes (rank and verify; fix all that apply in one image)

1. **ATC PHY not configured.** The Apple Type-C PHY (`/arm-io/atc-phy0..2`) needs its tunables
   and USB2 PHY enabled before the DWC3 sees a link. m1n1 already has `kboot_atc.c` (used for
   Linux) and Asahi has `drivers/phy/apple/atc.c`. Apply the ADT `tunable_*` properties and
   power the PHY (pmgr domains of `atc-phy*` and `usb-drd*`) in `prepare_usb_port()`.
2. **No VBUS / no data role.** The port controllers are TI TPS6598x (`hpm*` nodes on the SMC
   I2C buses). Without telling the HPM to source VBUS and act as DFP, attached devices never
   power up. m1n1 has `tps6598x.c`; Asahi uses `drivers/usb/typec/tipd`. Put each port into host
   (DFP) + source mode at boot.
3. **DWC3 mode/PHY selection.** Verify GCTL PRTCAPDIR=host, GUSB2PHYCFG (UTMI width, suspend
   bits) and that USB3 PIPE is either brought up or explicitly disabled so the core does not wait
   for a USB3 link. Compare with `usb_dwc3.c` and Asahi `dwc3-apple` glue.
4. **DART/IRQ plumbing.** Confirm DART translations cover the xHCI DMA ranges Windows uses (all
   guest RAM, not only the low window) and that the IRQs actually reach the guest (vGIC route
   capacity is `HV_IRQ_DYNAMIC_ROUTE_CAPACITY` in `hv_irq_routes.h`; the GPU now uses 9 routes).
5. **Guest RAM vs device windows.** Keep `J316S_SETUP_RAM_SIZE` 10 GiB: 12 GiB overlapped
   xHCI1/DART IPAs (Wifi-013 failure). The packer check `tests/test_pack_j316s_layout.py` must pass.

Use the live ADT already captured at
`.build/j316s-gpu/evidence/J316s-GPU-001-20261109T124308Z/adt.txt` (stub 13.5 ADT) for
`atc-phy*`, `usb-drd*`, `dart-usb*`, `hpm*` and their `tunable_*`, `reg`, `interrupts`.

## Implementation steps

1. Read the code above plus Asahi `atc.c`, `tipd/core.c`, `dwc3/` Apple glue (fetch raw files
   from github.com/AsahiLinux/linux, branch `asahi`, into `/Users/elfaria/win11/toolchains/asahi-ref`).
2. In m1n1 `hv_j316s_setup.c` (behind the existing USB flag): power ATC+DRD domains, apply ATC
   tunables, configure TPS6598x for host/source, configure DWC3, then hand xHCI to the guest.
   Log every step with `printf` (it lands in the persistent log, see below).
3. Keep all three ports; if one port fails its preflight, skip only that port.
4. Build: `PATH=$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/bin:/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH`,
   then `scripts/build-j316s-gpu.sh` (rename the image series to `J316s-USB-NNN` in a copy of
   the script; keep `J316S_GPU=1` and the GPU probe log). Mu only needs rebuilding if ACPI
   changes (command and PATH are in `investigation/J316S_GPU_PORT.md`).
5. Offline checks: `python3 -m unittest tests.test_j316s_gpulog`, the packer layout tests, and a
   zero-warning m1n1 build for files you touched.
6. Install with a copy of `scripts/install-j316s-gpu-esp.sh` pinned to the new image (rollback =
   Wi-Fi-017 or GPU-002), run via the terminal tab with sudo.
7. Ledger: append a `J316s-USB-NNN` entry to `investigation/EXPERIMENTS.md` before install
   (hypothesis, changes, hashes, rollback) and after install (readback).

## Evidence (so the next iteration needs no extra boot)

- m1n1 writes its console to ESP `::m1n1/gpulog.bin` (J316S_GPU_PROBE). Read after boot with
  `sudo sh scripts/read-j316s-gpulog.sh <IMAGE-NAME>`. Make sure your USB printf lines are in it.
- Windows side: add a USB section to `drivers/apple-agx/render-admission/j316s/gpu.ps1 collect`
  (pnputil USB tree, `Get-PnpDevice -Class USB`, USB events) so the boot task writes it to
  J316SOURCE `::/AppleGPU/logs`, then restage with `scripts/stage-j316s-agx.sh`.

## Done means

Image installed and readback-verified, ledger updated, rollback command stated, and a short
report: what changed, what the operator should plug in to test (a USB keyboard or flash drive in
each USB-C port via adapter), and how you will read the evidence after their boot.
