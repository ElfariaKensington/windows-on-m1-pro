# J316s AGX bring-up: handoff (driver 30.0.2009.0, 2026-11-09)

## State (2026-11-11)

Build 30.0.2008.0 reached GPU power-on, ASC startup and a gfx-handoff
response. The raw `Wom1RetainedActivate` receipt then returned
`HV_AGX_RETAINED_OWNERSHIP` (status 4, flags 7). The installed GPU-003 loader
used a 40-bit UAT table mask and dropped the high bits of T6000 physical
addresses. The 42-bit rebuild returns status 0 in the host replay.
See J316s-GPU-004 in [EXPERIMENTS.md](EXPERIMENTS.md) for build and rollback.

On GPU-004, build 30.0.2009.0 completed activation (step 8), firmware
I/O, hwdata and all 207 context-0 maps. The shared 3-second deadline
expired during 3.96 seconds of mapping, before the first RTKit send.
Build 30.0.2010.0 passed RTKit management, endpoint startup and backend
start on hardware. Windows then rejected the 1728x1117 Mu POST framebuffer
with 0xc01e0002, leaving the adapter at Code 43. Build 30.0.2011.0 and GPU-005
implement the paired display correction. Collect `Wom1PostDisplayRoute`,
`Wom1StartStage`, `Wom1StartStatus`, scanout/present receipts and any new
WATCHDOG dump before changing the candidate. Verify the panel shows a
known frame and that the latch acknowledges the same swap.

## Known not done (will block rendering even after boot works)
- Render template (apple_agx_render_template.generated.c) is a J313 single-cluster capture; T6000 needs
  multi-cluster fields (see J316S_GPU_PORT.md "Findings").
- hwdata T6000 profile uses placeholder fuse-dependent fields patched from m1n1 (AgxHwdataMaterialize).
- GPUVA leaf accounting now uses the platform 42-bit mask in m1n1 commit
  4c88ab14. The above-1-TiB replay passes. GPU-005 predates that commit.

## Gotchas
- Build is macOS-only (clang-cl/lld, pinned WDK in .build/j316s-fresh-input). `FORCE=1` is required after any header edit
  (build-clang.sh skips .obj newer than its .c). Loop: bump DriverVer in AppleAgxJ316s.inf,
  `FORCE=1 AGX_VERSION_BUILD=<n> sh scripts/package-j316s-agx.sh`, `sudo sh scripts/stage-j316s-agx.sh`.
- Install: J316SOURCE `Install Apple GPU.cmd` (admin). It sets one-shot `G3Armed=1`, installs, starts once.
  Every later boot is Code 43 by design until reinstalled.
- BSOD triage without symbols: parse minidump (bugcheck at 0x38, CONTEXT at 0x348, pc at +0x108), map pc to
  AppleAgxRenderAdmission.sys RVA, `llvm-objdump -d` the signed .sys. ntoskrnl for this build: base.wim index 1.
