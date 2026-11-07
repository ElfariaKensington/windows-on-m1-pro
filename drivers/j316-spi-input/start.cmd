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
drvload X:\J316Input\J316SpiInput.inf > X:\J316Input\install.log 2>&1
set "load_result=%ERRORLEVEL%"
echo J316 SPI input install result: %load_result%
echo This result reports package loading. Hardware startup status is below.
type X:\J316Input\install.log
pnputil /scan-devices > X:\J316Input\scan.log 2>&1
pnputil /enum-devices /deviceid "ACPI\APPL0001" /drivers /services /stack /resources > X:\J316Input\device.log 2>&1
X:\J316Input\J316Tools.exe > X:\J316Input\input.log 2>&1
type X:\J316Input\input.log
start "J316 input log" /b cmd /c X:\J316Input\logs.cmd
echo.
echo Keep the phase and status values above if input does not work.
echo Phase 10 means running; phase 11 means resource rejection.
echo Windows Setup opens in 20 seconds. This diagnostics window stays open.
X:\J316Input\J316Tools.exe /wait
X:\sources\setup.exe
cmd.exe
