# J316 synthetic Wi-Fi Ethernet miniport

Classic NDIS 6.30 for ARM64 Windows 11, `ACPI\APPL0010`, service `J316Wifi`.
ARM64 WDK headers require NDIS >=6.30. No KMDF or native WLAN interface.

## Build

Run `./build.sh`. It cross-compiles with clang-cl/lld and the existing pinned
`.build/j316s-fresh-input` WDK/SDK 10.0.28000.0, with `/W4 /WX /GS` and
`GsDriverEntry`. Output: `build/J316Wifi.sys` and accompanying package files.
`JW_PACKAGES`, `JW_OUTPUT`, `CLANG_CL`, `LLD_LINK` override paths.
No catalog, signing, installation, hardware run, or offline staging is performed.

## Packet and lifecycle contract

`bridge.h` is authoritative and unchanged. ACPI must allocate memory at
`JW_BASE=0x410000000`, length >=`JW_SIZE=0x80000`; magic/version must match.
The host owns physical Wi-Fi, initialization, DMA, interrupts, firmware, power
and recovery. Windows owns only the synthetic Ethernet endpoint.

The requested 5ms periodic timer publishes TX and copies RX with register
accessors and memory barriers. Every slot is checked for 14..1514-byte Ethernet
length; unsigned counter occupancy must remain <=64. The driver never resets
host-owned counters. Counter corruption disconnects until reinitialization.
Timers are subject to Windows timer granularity and scheduling.

TX completes only after all NBs in an NBL have been copied into a bounded
64-frame private queue. Fragmented MDLs are copied with `NdisGetDataBuffer`;
short frames are zero-padded to 60 bytes. A multi-NB NBL is accepted atomically
or failed; queue exhaustion returns resources. No NBL is retained. Queue copies
can be dropped on link loss/pause/power/reset after send success, as with other
buffered Ethernet devices. TX counters count ring publication, not over-air ACKs.
RX uses a preallocated nonpaged MDL/NBL and synchronous `RESOURCES` indications;
directed/broadcast/multicast/promiscuous filtering includes DHCP and IPv6 traffic.
Pause/halt stop admission, cancel/drain the periodic timer, wait for accepted
send handlers to finish completion callbacks, then discard copies. Low-power
OIDs also drain the timer and sends; no register access occurs in D1..D3.

No checksum/segmentation offload, RSS, VLAN, jumbo frames, wake or radio-rate
telemetry is advertised. The synthetic link rate is 1 Gbit/s. Ethernet/NDIS
configuration and statistics OIDs are supported; real DHCP/TCP still depend on
the host's associated Wi-Fi and Ethernet packet translation.

## Credentials

Run `configure.ps1` interactively in elevated Windows PowerShell after external
installation. It selects service `J316Wifi` / `ACPI\APPL0010`, prompts for a
1..32-byte UTF-8 SSID and a SecureString PSK (8..63 printable ASCII characters),
writes adapter NDIS `REG_SZ` values `SSID`/`PSK`, restarts the adapter, and removes
`PSK` in `finally`. Open networks and raw 64-hex PSKs are not supported by the
host JOIN contract. Missing PSK means no new JOIN, preserving host association.

The driver reads both values synchronously during initialization, closes NDIS
configuration, holds a bounded private copy until READY, and issues JOIN only
when COMMAND is idle. It erases its credential copy immediately after issuing
JOIN, then erases MMIO staging when the host clears COMMAND after copying.
COMMAND_STATUS is 0 for queued or a raw negative error; link readiness comes
only from STATUS. No credential logging is implemented. Do not run the script
under transcription or value-recording tracing. Temporary registry writes can
survive forced termination and leave journal/backup remnants; managed plaintext
copies cannot be guaranteed erased. This is not a durable credential store.
If halted before COMMAND consumption, host cleanup owns the remaining mailbox.

## Diagnostics and integration checkpoint

When firmware is unready, the device still enumerates disconnected. If MAC
registers are invalid at initialization it uses a local diagnostic MAC and never
transfers packets; restart the adapter after host recovery to acquire its real
MAC. Read-only vendor OIDs return raw ULONG register snapshots (last running poll,
or initialization snapshot while paused); errors are interpreted as signed int32:

| OID | Value |
| --- | --- |
| `0xff000201` | PHASE |
| `0xff000202` | ERROR |
| `0xff000203` | STATUS |
| `0xff000204` | COMMAND_STATUS |

Inspected contracts: `bridge.h`, sibling SPI input build script, pinned WDK
`ndis.h`/`ntddndis.h`, and host `m1n1_windows/src/wifi_bcm.h`. No Asahi physical
driver protocol is reproduced in Windows. Mu ACPI and host ABI integration are
owned by main. Smallest hardware checkpoint: enumerate APPL0010, inspect MAC and
phase/error, configure JOIN, observe link transition, then DHCP and TCP traffic.
Main owns signing, staging, hardware validation and rollback (disable/remove this
adapter package without changing the independently verified input package).

Primary Microsoft references:

- [Miniport initialization](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/initializing-a-miniport-adapter)
- [Timer cancellation and periodic DPC draining](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndiscanceltimerobject)
- [Synchronous resource receive ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismindicatereceivenetbufferlists)
- [Pause requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_pause)
- [Power OIDs](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-pnp-set-power)
- [NDIS statistics](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-gen-statistics)
