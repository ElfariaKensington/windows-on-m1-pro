# J316s internal Windows installation

The input-qualified boot profile exposed a 6.4 GB diagnostics container and only
a RAM-backed `boot.wim`. It did not expose the installation SWMs or a sufficiently
large target. The Load Driver screen was therefore not proof of a missing NVMe
driver: the existing mediated NVMe disk was already mounted as `J316S_DIAG`.

`layout.py` builds a new regular 80 GB image with 4096-byte sectors, matching the
Apple ANS namespace and existing emulated NVMe controller. It contains a 300 MiB
FAT32 ESP, 16 MiB MSR, about 65.2 GiB unallocated for Windows, and a 9 GiB FAT32
`J316SOURCE` partition. The latter contains both installation SWMs, Setup files,
the input package and OEM first-boot files. It has no fallback EFI bootloader.
The Microsoft image and project input-driver binaries are unchanged.

The host container uses type `CBAE986F-5E92-48D2-AEF2-326CB78B01EB`, independently
of the diagnostics type. The m1n1 installation profile validates both physical
GPT copies and exposes exactly one matching container of at least 64 GiB.
Every translated read/write remains bounded by that container. The original
diagnostics profile and full-disk backend retain their existing selectors.
No native Apple NVMe register or protocol implementation changes.

`provision.py` prints its plan by default. `--execute` requires the exact J316s
model, macOS physical-store UUID/size, EFI UUID/size and enough APFS resize room.
It saves the complete partition map (plus raw GPT sectors when macOS permits),
shrinks APFS using `diskutil`, and adds the private
partition with `diskutil addPartition`. Existing partition offsets, sizes and
UUIDs must remain unchanged except for the authorized macOS shrink. The 256 MiB
margin accommodates macOS' automatic helper partition for unknown GPT types.
The new container must be wholly inside the freed range and unmounted.

`deploy.py` requires the provisioning receipt and image hash. It rechecks the
new partition's UUID/type/offset/size/internal location and unchanged initial
edges before writing. It then reads back and hashes all 80 GB. It cannot target
APFS, EFI, a whole disk or an external disk. Existing EFI deployment resolves
its stable UUID because BSD slice numbers may change when adding partitions.

Build the source image with:

```sh
python3 tools/j316s_install/layout.py --source /path/to/ARM64-media \
  --package /path/to/signed-input-package --output /path/to/new-disk.img
```

Build the input WinPE payload with `drivers/j316-spi-input/build-image.py
--windows-install`; its existing required arguments still apply. The optional
`--boot-wim-output` retains the exact serviced WIM for VM validation. Startup
locates the marked internal source and launches Setup with explicit
`/installfrom:<first.swm>` and `/unattend:<file>` arguments. This media's legacy
Setup parser requires colon-separated values. TPM/Secure Boot compatibility
options are present in its own `winsetup.dll`; the isolated project profile has
no TPM or Secure Boot. Windows 11 Pro is selected with its public, non-activating default installation
key. Account, license and partition choices remain interactive. Select the unallocated area; keep the source, ESP and MSR.

OEM deployment copies `C:\J316Input` into the new system. A specialize command
imports the same development certificate, enables test signing for the installed
BCD and stages the input package, followed by a supported `WillReboot=Always`
restart. This happens before account setup. Full Windows includes the Microsoft
HID INF missing from WinPE. OOBE hides online-account/wireless pages so account
creation can proceed offline. No account or password is pre-created.

The current Mu storage build already tries the installed NVMe
`\EFI\Microsoft\Boot\bootmgfw.efi` before its RAM-backed Setup image. After the
installation restarts, selecting the same Apple boot-picker entry continues the
installed system. The RAM-backed installer remains available while the inner
ESP has no installed Windows loader. Existing rollback images remain separate.

Validation includes checksummed primary/backup GPT geometry, minimum Windows
space, protected-partition preservation, wrong-device refusal, both disk-role
selectors and bounds, plus an ARM64 Windows installation on a disposable 4Kn
NVMe image. The VM's automation and root-enumerated HID probe are test-only;
neither is included in the native image.

Primary references: the host `diskutil(8)` manual;
[UEFI GPT](https://uefi.org/specifications);
[Windows UEFI partition layout](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/configure-uefigpt-based-hard-drive-partitions);
[Setup options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/windows-setup-command-line-options);
[specialize restart semantics](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-deployment-runsynchronous-runsynchronouscommand-willreboot).
