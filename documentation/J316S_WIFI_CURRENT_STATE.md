# J316s internal Wi-Fi current state

Windows is installed and has reached OOBE. Input packages were subsequently
installed offline as OEM0/OEM1 (J316S-INPUT-REPAIR-003); native result pending.
Claude's v2 plan predates this installation and has stale component pins.

Internal Wi-Fi is **not implemented or installed**. This checkpoint implements
only read-only collection and exact firmware preparation. No WLAN device is
exposed to the current Windows profile; no Broadcom firmware/ring/association
backend or guest network adapter exists. Do not describe firmware extraction as
working Wi-Fi or stage a generic Windows Broadcom driver.

Live collection: MacBookPro18,1; BCM4387C2; madagascar; antenna X3;
OTP board parameters M=TPHN, V=u, m=4.7. PERST is GPIO phandle0x88/pin4;
CLKREQ is the same controller/pin0/function2. apcie is apcie,t6000;
DART is dart,t6000 at0x581008000, IRQ1271. Hardware facts are from the live
IORegistry, not guessed register writes. Module serial/complete OTP not retained.

Run `python3 tools/wifi/prepare_j316s.py` from macOS. Private firmware,
calibration, station MAC and manifest stay under `.build/j316s-wifi/local/`.
The collector follows Apple's actual board symlinks: madagascar-X3.trx resolves
to maldives.trx. NVRAM selection uses the measured module revision, not the
first file in the directory. No .sig exists for this firmware set. Files mode0600.
No SSID, password, keychain or radio configuration was read or changed.

The user permits internal Wi-Fi only and has no second computer. There is no
active proxy/guest launcher and only macOS Bluetooth/debug serial nodes.
The plan's interactive two-computer proxy track is therefore unavailable.
Next native checkpoint needs an autonomous, bounded, opt-in PCIe port0 probe
with durable results accessible after returning to macOS. Do not change normal
boot or claim association without that checkpoint. After link/ID14e4:4433,
validate DART SID1/RID mapping and firmware boot before implementing packet
rings and the guest adapter. Codex installation/online operation remains undone.

References inspected: current m1n1 src/pcie.c, smc.c, kboot.c:dt_set_wifi,
hv_j316s_setup.c, and current Asahi sources:
- https://github.com/AsahiLinux/asahi-installer/blob/main/asahi_firmware/wifi.py
- https://github.com/AsahiLinux/linux/blob/asahi/drivers/pci/controller/pcie-apple.c
- https://github.com/AsahiLinux/linux/blob/asahi/drivers/gpio/gpio-macsmc.c
- https://github.com/AsahiLinux/linux/blob/asahi/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c
- brcmfmac firmware.c, msgbuf.c, cfg80211.c in the same tree.

License decision: independent collector; no external implementation copied.
Linux GPL controller code remains reference only. Proprietary blobs stay local.
