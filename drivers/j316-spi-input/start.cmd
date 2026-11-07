@echo off
setlocal EnableExtensions
mode con cols=100 lines=45
title J316 built-in keyboard and touchpad diagnostics
wpeinit
mountvol /E > X:\J316Input\mountvol.log 2>&1
(
    echo automount enable
    echo rescan
    echo list volume
    echo exit
) > X:\J316Input\volumes.txt
diskpart /s X:\J316Input\volumes.txt > X:\J316Input\volumes.log 2>&1
X:\J316Input\J316Tools.exe /trust > X:\J316Input\trust.log 2>&1
pnputil /add-driver X:\J316Input\J316VhfInput.inf > X:\J316Input\hid-bridge-install.log 2>&1
set "hid_result=%ERRORLEVEL%"
drvload X:\J316Input\J316SpiInput.inf > X:\J316Input\install.log 2>&1
set "load_result=%ERRORLEVEL%"
echo J316 SPI input install result: %load_result%
echo This result reports package loading. Hardware startup status is below.
type X:\J316Input\install.log
pnputil /scan-devices > X:\J316Input\scan.log 2>&1
pnputil /enum-devices /deviceid "ACPI\APPL0001" /drivers /services /stack /resources > X:\J316Input\device.log 2>&1
start "J316 input log" /b cmd /c X:\J316Input\logs.cmd
set "hid_bound="
for /l %%N in (1,1,7) do (
    if not defined hid_bound (
        X:\J316Input\J316Tools.exe /bind > X:\J316Input\hid-bridge-bind.log 2>&1
        if not errorlevel 1 set "hid_bound=1"
    )
    cls
    echo J316 internal SPI startup sample %%N of 7 - refreshing every 3 seconds
    echo Package load result: %load_result%
    echo HID bridge package result: %hid_result%
    type X:\J316Input\hid-bridge-bind.log
    X:\J316Input\J316Tools.exe
    echo.
    echo Keep the phase and status from sample 7 if input does not work.
    echo Phase 10 means SPI/VHF source started; phase 11 means resource rejection.
    echo Phases 12-16 locate SPI/GPIO setup and the three reset delays.
    X:\J316Input\J316Tools.exe /delay
)
pnputil /enum-devices /class HIDClass /services /stack > X:\J316Input\hid-children.log 2>&1
pnputil /enum-devices /class Keyboard /services /stack > X:\J316Input\keyboard.log 2>&1
pnputil /enum-devices /class Mouse /services /stack > X:\J316Input\mouse.log 2>&1
X:\sources\setup.exe
cmd.exe
