@echo off
setlocal EnableExtensions
wpeinit
X:\J316Input\J316Tools.exe /trust > X:\J316Input\trust.log 2>&1
drvload X:\J316Input\J316SpiInput.inf > X:\J316Input\install.log 2>&1
set "load_result=%ERRORLEVEL%"
echo J316 SPI input install result: %load_result%
type X:\J316Input\install.log
pnputil /scan-devices > X:\J316Input\scan.log 2>&1
pnputil /enum-devices /deviceid "ACPI\APPL0001" /drivers /services /stack /resources > X:\J316Input\device.log 2>&1
X:\J316Input\J316Tools.exe > X:\J316Input\input.log 2>&1
type X:\J316Input\input.log
start "J316 input log" /b cmd /c X:\J316Input\logs.cmd
X:\sources\setup.exe
cmd.exe
