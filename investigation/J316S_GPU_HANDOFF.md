# J316s AGX bring-up: handoff (driver 30.0.2009.0, 2026-11-09)

## State (2026-11-11)

Build 30.0.2008.0 reached GPU power-on, ASC startup and a gfx-handoff
response. The raw `Wom1RetainedActivate` receipt then returned
`HV_AGX_RETAINED_OWNERSHIP` (status 4, flags 7). The installed GPU-003 loader
used a 40-bit UAT table mask and dropped the high bits of T6000 physical
addresses. The 42-bit rebuild returns status 0 in the host replay.
See J316s-GPU-004 in [EXPERIMENTS.md](EXPERIMENTS.md) for build and rollback.

Build 30.0.2009.0 records `Wom1ActivateStep` and `Wom1ActivateA/B` under
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\APPL0002\0\Device Parameters`.
On the next Windows run, capture the raw activation receipt, the step
(8 means arena adoption and relocation completed), firmware I/O and context-0
receipts, and the adapter status.

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
