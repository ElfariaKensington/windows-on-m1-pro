# J316 native Wi-Fi (WiFiCx) and the Wi-Fi mode switch

KMDF 1.33 + NetAdapterCx 2.2 + WiFiCx 1.0 client for `ACPI\APPL0010`. Windows shows a real
Wi-Fi adapter: scanning, the network list, and connecting from Settings or `netsh wlan`.

m1n1 still owns PCIe, DART, DMA, firmware and rings. The driver switches the bridge to
native mode (`bridge.h`, FEATURES bit 0). It then sends Broadcom firmware commands
through the mailbox and receives raw firmware events, as brcmfmac's cfg80211 layer does.
Windows runs the WPA2 supplicant: EAPOL goes over the data path and keys are installed
with `wsec_key`.

Supported: open networks and WPA2-Personal with CCMP pairwise and a CCMP or TKIP group
cipher (mixed WPA/WPA2 routers), 2.4 and 5 GHz.
Not supported: TKIP pairwise, WEP, WPA3-SAE, 802.1X, PMF, Wi-Fi Direct, wake, power save.

## Two modes, one switch

| Mode | Driver | Who connects | Use when |
| --- | --- | --- | --- |
| Wi-Fi | J316WifiCx (this folder) | Windows (Settings) | normal use, any WPA2 network |
| Ethernet | J316Wifi (`drivers/j316-wifi`) | m1n1 before Windows starts (credentials baked into the boot image; `ethernet\configure.ps1` for another network) | fallback if Wi-Fi mode misbehaves |

`switch-mode.ps1 [wifi|ethernet|status|menu]` removes every staged J316 Wi-Fi package and
installs the selected one. Switching to Ethernet needs a restart (the host stays in native
mode until the next boot); a restart is also recommended after switching to Wi-Fi.

## Build, sign, stage (macOS)

```
sh scripts/package-j316s-wificx.sh        # J316WifiCx package (bump DriverVer first)
sh scripts/package-j316s-wifi-bridge.sh   # J316Wifi package
sudo sh scripts/stage-j316s-wificx.sh     # J316SOURCE ::/J316WifiCx (both modes)
```

Signing certificate: `.build/j316s-wificx/signing`, CN=J316 WiFi Development, valid
2020-2036 (the guest boots with a 2024 clock until time sync). In Windows run
`J316WifiCx\Wi-Fi Mode.cmd` (or `Use Wi-Fi.cmd` / `Use Ethernet.cmd`) as administrator.
Needs m1n1 image J316s-Wifi-017 or later. Driver log: `%SystemRoot%\Temp\J316WifiCx.log`.
Evidence collection from macOS (read-only): `sudo sh scripts/collect-j316s-wifi-evidence.sh`.

## Contracts learned the hard way

- The peer-address TX demux of a station adapter must have Range 1. WifiAdapterInitialize
  asserts it; Range 8 (0.5.1/0.5.2) bugchecked with 0x7E/c0000420 in WifiCx.sys on every load.
- WiFiCx does not drive the NetAdapterCx media state. The client sets it with
  NetAdapterSetLinkState after association; otherwise Windows never transmits the
  EAPOL 2/4 reply and the connection fails with "Dynamic key exchange did not succeed".
