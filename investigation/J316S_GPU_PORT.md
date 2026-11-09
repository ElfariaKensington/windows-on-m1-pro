# J316s (T6000) GPU port of the J313 AGX stack — working plan

Read this first after a context reset. Operator decisions (2026-11-09): port the
existing J313 stack (do not rewrite), do everything in one image, no further
log-only boots, MSVC CRT/SDK license accepted for the UMD build (xwin).

## Inputs
- Live 13.5 GPU contract: `config/j316s-agx.json` (from J316s-GPU-001 log,
  evidence `.build/j316s-gpu/evidence/J316s-GPU-001-20261109T124308Z/adt.bin`).
- Toolchains: `/Users/elfaria/win11/toolchains/msvc-sysroot` (xwin, ARM64 CRT+SDK),
  `/Users/elfaria/win11/toolchains/mesa` (pinned 9aa1215f), WDK 10.0.28000 nuget
  under `.build/j316s-fresh-input`, Homebrew llvm clang-cl/lld, meson.
- Asahi reference sources: `/Users/elfaria/win11/toolchains/asahi-ref`.

## Architecture (J313 production, to be reproduced on J316s)
- m1n1 owns: power broker page at IPA 0x300000000 (power, local-reserve receipt
  at +0xd00, pool window +0xe00, hwdata receipt +0xc00, fw-io +0x800, retained
  root +0x600, GPUVA v5 +0x700, scanout +0x400, config snapshot +0x100),
  gpu-region hook, retained firmware root in gfx-shared-region, IRQ routes
  880..888, scanout IRQ 889. Entry: `hv_agx_g2_resources_map()`.
- J313 guest RAM is IPA==PA; J316s is not (IPA 0x850000000 -> heap PA). GPU
  memory must come from the identity-mapped local reserve; system-memory pages
  use HVC 0x4d31 IPA->PA.
- KMD: `drivers/apple-agx/render-admission` (+ `shared`), UMD:
  `render-admission/umd` + Mesa native runtime (asahi gallium + d3d10/11/12).

## T6000 deltas
- PAs >= 1<<40: every 40-bit PA limit -> 42 (76 sites, see grep in ledger).
- UAT output 42 bits (initdata phys_mask 0x3ffffffc000).
- hwdata: CHIP_INFO[0x6000] (from T6001 + Asahi t600x.rs overrides); fuse
  dependent fields (max_power*, core/sram leak, rel_max_powers) patched at boot
  from m1n1 kboot_gpu calc_power_t600x.
- io_mappings: T6000 list (mcc_count 4).
- Power: GFX domain 0x128 (virtual gates GFX-ASC/SGX/BUSY).
- Stage-2: map only sgx0/sgx1/asc0 windows; handoff identity.
- Presentation: no DCP owner on J316s; scanout broker must present into the
  m1n1-drawn guest framebuffer path.

## Findings 2026-11-09 (port session 1)
- hwdata: Asahi t600x unk_coef_a/b static per cluster ([9.838, 9.819], [13, 13] for T6000);
  fuse-dependent: max_power*, rel_max_powers, unk_3cf4 (core leak), unk_3d14 (sram leak).
  m1n1 kboot_gpu calc_power_t600x computes them (fuses 0x2922bc1b8/0x2922bc1cc).
  Plan: offline profile with placeholders, field offsets by differential generation,
  m1n1 publishes values at broker+0xc40, KMD patches in AgxHwdataMaterialize.
- RegionC (shared/src/apple_agx_regionc.c) is a J313 word list: regenerate with
  proxyclient InitData_RegionC(sgx, CHIP_INFO[0x6000]).
- Render template (shared/src/apple_agx_render_template.generated.c, 6 MB, 75
  objects) is a J313 hardware capture (EXP208); generator script absent. T6000 needs
  multi-cluster render fields (tiling_control 0xa540, core masks, clustering meta
  buffers per Asahi queue/render.rs). Rebuild from proxyclient objects offline.
- Production ACPI (J313AppleAgxAbiAdmission.asl.inc): SGX 64 MiB, gpu-region,
  handoff, broker page, local reserve via receipt, IRQ 889 (scanout).

## Status
- [x] Live contract + generator (`tools/generate_j316s_agx_g2_contract.py`)
- [x] T6000 hwdata profile (static, kboot_gpu power model) + RegionC
- [x] m1n1 J316S_GPU: services, stage-2, IRQs, identity reserve, panel-copy scanout
- [x] Mu DSDT AGX0/APPL0002
- [x] KMD: 42-bit, J316s defines, clang-cl build (render-admission/build-clang.sh)
- [x] UMD: Mesa native runtime via ported J313 scripts (.build/j316s-agx/mesa-scripts), umd/build-clang.sh
- [x] Package (scripts/package-j316s-agx.sh), stage (scripts/stage-j316s-agx.sh), ESP GPU-002 installed
- [ ] Hardware result of GPU-002; render template chip fields (EXP208 is single-cluster J313)
