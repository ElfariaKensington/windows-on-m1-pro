@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "STATE=X:\J316sInput"
if not exist "%STATE%" md "%STATE%"

(
    echo J316s Windows Setup input diagnostics
    echo Boot time: %DATE% %TIME%
    echo WinPE: %PROCESSOR_ARCHITECTURE%
) > "%STATE%\boot.log"
wpeinit >> "%STATE%\boot.log" 2>&1
set "WpeinitResult=%ERRORLEVEL%"
echo wpeinit exit code: %WpeinitResult%>> "%STATE%\boot.log"
mountvol /E > "%STATE%\mountvol-enable.log" 2>&1

(
    echo list disk
    echo list volume
    echo exit
) > "%STATE%\diskpart.txt"
(
    echo automount enable
    echo rescan
    echo list disk
    echo list volume
    echo exit
) > "%STATE%\diskpart-rescan.txt"
diskpart /s "%STATE%\diskpart.txt" > "%STATE%\disk-layout.log" 2>&1
diskpart /s "%STATE%\diskpart-rescan.txt" > "%STATE%\disk-layout-rescan.log" 2>&1
call "%STATE%\persist_logs.cmd"
pnputil /enum-devices /connected /deviceids /drivers /services /stack /interfaces /resources > "%STATE%\pnp-all.log" 2>&1
pnputil /enum-devices /class Keyboard /deviceids /drivers /services /stack /interfaces /resources > "%STATE%\pnp-keyboard.log" 2>&1
pnputil /enum-devices /class HIDClass /deviceids /drivers /services /stack /interfaces /resources > "%STATE%\pnp-hid.log" 2>&1
pnputil /enum-devices /deviceid "ACPI\APPL0001" /deviceids /drivers /services /stack /interfaces /resources > "%STATE%\pnp-appleinput.log" 2>&1
dism /online /get-drivers /format:table > "%STATE%\driver-store.log" 2>&1
sc query type= driver state= all > "%STATE%\driver-services.log" 2>&1

X:\J316sInput\TrustAppleInput.exe > "%STATE%\trust.log" 2>&1
set "TrustResult=%ERRORLEVEL%"
echo %TrustResult% > "%STATE%\trust-result.txt"
drvload X:\J316sInput\AppleInput.inf > "%STATE%\drvload.log" 2>&1
set "DrvloadResult=%ERRORLEVEL%"
echo %DrvloadResult% > "%STATE%\drvload-result.txt"
pnputil /scan-devices > "%STATE%\pnp-scan.log" 2>&1
pnputil /enum-devices /connected /deviceids /drivers /services /stack /interfaces /resources > "%STATE%\pnp-after-drvload.log" 2>&1
pnputil /enum-devices /deviceid "ACPI\APPL0001" /deviceids /drivers /services /stack /interfaces /resources > "%STATE%\pnp-appleinput-after-drvload.log" 2>&1
sc query AppleInput > "%STATE%\service-after-drvload.log" 2>&1
sc qc AppleInput >> "%STATE%\service-after-drvload.log" 2>&1
dism /online /get-drivers /format:table > "%STATE%\driver-store-after-drvload.log" 2>&1
(
    echo TrustAppleInput exit code: %TrustResult%
    echo drvload exit code: %DrvloadResult%
) >> "%STATE%\boot.log"
copy /y "%SystemRoot%\INF\setupapi.dev.log" "%STATE%\setupapi.dev.log" >nul 2>&1
copy /y "%SystemRoot%\INF\setupapi.offline.log" "%STATE%\setupapi.offline.log" >nul 2>&1
copy /y "%SystemRoot%\Panther\setupact.log" "%STATE%\setupact.log" >nul 2>&1
copy /y "%SystemRoot%\Panther\setuperr.log" "%STATE%\setuperr.log" >nul 2>&1
wevtutil qe System /c:500 /rd:true /f:text > "%STATE%\system-events.log" 2>&1
call "%STATE%\persist_logs.cmd"
start "" /b cmd.exe /c "%STATE%\collect_input.cmd"
:setup
X:\sources\setup.exe
call "%STATE%\persist_logs.cmd"
