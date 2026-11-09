# J316s AGX bring-up: handoff (driver 30.0.2009.0, 2026-11-09)

## State
Driver loads (Code 43, not 39/crash). StartDevice passes: resources, G3Armed gate, UAT/memory start,
power (GFX domain on), UAT ready, ASC CPU start, **gfx-handoff exchange OK** (firmware answers).
Fails in provider bootstrap phase `ROOTS` (Wom1ProviderBootFailurePhase=2) after retained-root
`Activate` succeeded (Wom1RetainedActivate flags 7, SystemVa 0xffffffa080000000, 0x4000).
`Wom1FirmwareIoResult` is never written, so the failure is between Activate and
`AgxFwIoReadManifest` in `AdmissionRetainedActivate` (backend_platform_windows.c): QueryArena(COMMAND),
`AppleAgxInitdataMemoryApplyCommandArena`, `...BindRelocationObjects`, `AppleAgxApplyRelocations`.
2009 records `Wom1ActivateStep` (0 gate,1 start,2 response,3 query,4 arena,5 apply,6 bind,7 reloc,8 done)
+ `Wom1ActivateA/B` in `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\APPL0002\0\Device Parameters`. Read these first.
Failure cleanup then returns STATUS_DEVICE_BUSY (retained firmware cannot be stopped); no BSOD since 2008.

## Known not done (will block rendering even after boot works)
- Render template (apple_agx_render_template.generated.c) is a J313 single-cluster capture; T6000 needs
  multi-cluster fields (see J316S_GPU_PORT.md "Findings").
- hwdata T6000 profile uses placeholder fuse-dependent fields patched from m1n1 (AgxHwdataMaterialize).
- m1n1 hv_agx_gpuva_v5.c GPUVA_PA_MASK is still 40-bit (0x000000ffffffc000); needs APPLE_AGX_UAT_OA_MASK.
  m1n1 submodule changes (m1n1_windows, mu) are NOT pushed.

## Gotchas
- Build is macOS-only (clang-cl/lld, pinned WDK in .build/j316s-fresh-input). `FORCE=1` is required after any header edit
  (build-clang.sh skips .obj newer than its .c). Loop: bump DriverVer in AppleAgxJ316s.inf,
  `FORCE=1 AGX_VERSION_BUILD=<n> sh scripts/package-j316s-agx.sh`, `sudo sh scripts/stage-j316s-agx.sh`.
- Install: J316SOURCE `Install Apple GPU.cmd` (admin). It sets one-shot `G3Armed=1`, installs, starts once.
  Every later boot is Code 43 by design until reinstalled.
- BSOD triage without symbols: parse minidump (bugcheck at 0x38, CONTEXT at 0x348, pc at +0x108), map pc to
  AppleAgxRenderAdmission.sys RVA, `llvm-objdump -d` the signed .sys. ntoskrnl for this build: base.wim index 1.
